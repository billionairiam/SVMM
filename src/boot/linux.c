#include "boot/linux.h"

#include "ablation.h"
#include "memory.h"

#include <asm/bootparam.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BZIMAGE_MAX_SIZE (512u * 1024u * 1024u)
#define BOOT_PROTOCOL_MIN 0x020cu
#define E820_RAM 1u
#define E820_RESERVED 2u
/* setup_header 在 bzImage 文件和 boot_params（zero page）中都位于偏移 0x1f1。 */
#define SETUP_HEADER_OFFSET 0x1f1u
/* 本加载器最后读取的字段是 init_size（0x260–0x263），启动头至少要到 0x264。 */
#define SETUP_HEADER_MIN_END 0x264u

_Static_assert(offsetof(struct boot_params, hdr) == SETUP_HEADER_OFFSET,
               "unexpected Linux boot header layout");
_Static_assert(offsetof(struct boot_params, e820_table) == 0x2d0,
               "unexpected Linux e820 layout");
_Static_assert(SETUP_HEADER_OFFSET + offsetof(struct setup_header, init_size) +
                   sizeof(((struct setup_header *)0)->init_size) ==
                   SETUP_HEADER_MIN_END,
               "unexpected init_size offset");

/* alignment 必须是 2 的幂。 */
static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/*
 * 文件偏移 0x200 是一条两字节短跳转指令 `EB xx`（即 setup_header.jump，
 * 按小端读出后 xx 在高字节），xx 是以 0x202 为基准的 8 位相对位移。
 * 合法 bzImage 用它跳过启动头、到达 setup 代码入口，所以启动头在
 * 0x202 + xx 处结束。较新的协议版本会在末尾追加字段，启动头随之变长。
 */
static size_t setup_header_end(const struct setup_header *hdr)
{
    return 0x202u + (hdr->jump >> 8);
}

/*
 * 把一个 Linux x86 bzImage 读入宿主机内存并解析启动头。
 *
 * bzImage 由两部分组成：
 *   [引导扇区 + setup 代码][压缩内核载荷]
 *                            ^ setup_size，也是内核载荷的文件偏移
 *
 * 本函数只负责读取和验证镜像。把各部分复制进客户机内存的工作由
 * boot_linux_prepare() 完成。
 *
 * 启动头字段使用内核头文件 <asm/bootparam.h> 中 struct setup_header 的
 * 字段名。boot_params 中本加载器关心的协议偏移：
 *
 *   0x1e8  e820_entries           e820 条目数
 *   0x1f1  hdr.setup_sects        setup 扇区数
 *   0x200  hdr.jump               跳过启动头的短跳转，决定启动头长度
 *   0x202  hdr.header             魔数 "HdrS"
 *   0x206  hdr.version            启动协议版本
 *   0x210  hdr.type_of_loader     引导加载器 ID
 *   0x211  hdr.loadflags          LOADED_HIGH、CAN_USE_HEAP 等标志
 *   0x218  hdr.ramdisk_image      initramfs 客户机物理地址（由加载器填写）
 *   0x21c  hdr.ramdisk_size       initramfs 字节数（由加载器填写）
 *   0x228  hdr.cmd_line_ptr       命令行客户机物理地址
 *   0x22c  hdr.initrd_addr_max    initramfs 最后一个字节允许的最高地址
 *   0x230  hdr.kernel_alignment   可重定位内核的对齐要求
 *   0x234  hdr.relocatable_kernel 可重定位标志
 *   0x238  hdr.cmdline_size       镜像允许的最大命令行长度
 *   0x258  hdr.pref_address       内核首选运行地址
 *   0x260  hdr.init_size          早期初始化内存窗口大小
 *   0x2d0  e820_table             BIOS 风格物理内存表
 */
int boot_linux_load(const char *path, struct linux_image *image)
{
    /* 先清空输出，确保任意失败路径都能安全调用 boot_linux_free()。 */
    *image = (struct linux_image){ 0 };

    /* O_CLOEXEC 防止以后执行其他程序时把镜像文件描述符泄漏过去。 */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open bzImage");
        return -1;
    }
    /*
     * 在分配内存前检查文件大小：文件至少要包含到 init_size 为止的启动头，
     * 512 MiB 是本程序设置的安全上限。
     */
    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0 || statbuf.st_size < 0 ||
        statbuf.st_size > BZIMAGE_MAX_SIZE ||
        statbuf.st_size < SETUP_HEADER_MIN_END) {
        fprintf(stderr, "invalid bzImage file size\n");
        close(fd);
        return -1;
    }
    size_t size = (size_t)statbuf.st_size;
    uint8_t *data = malloc(size);
    if (!data) {
        perror("allocate bzImage buffer");
        close(fd);
        return -1;
    }
    /* read() 可能被信号中断或只读取一部分，因此循环直到读完整个文件。 */
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = read(fd, data + offset, size - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            perror("read bzImage");
            free(data);
            close(fd);
            return -1;
        }
        offset += (size_t)n;
    }
    close(fd);

    /*
     * 把文件偏移 0x1f1 处的启动头复制到本地结构体，之后用字段名访问，
     * 不再手写偏移。struct setup_header 在内核头文件里是 packed 的，
     * 字段布局与协议文档逐字节一致。文件比整个结构体短时，多出的尾部
     * 字段保持为 0（前面已保证文件至少包含到 init_size）。
     */
    struct setup_header hdr = { 0 };
    size_t hdr_bytes = size - SETUP_HEADER_OFFSET;
    memcpy(&hdr, data + SETUP_HEADER_OFFSET,
           hdr_bytes < sizeof(hdr) ? hdr_bytes : sizeof(hdr));

    /*
     * setup_sects：boot sector 后面还有多少个 setup 扇区。协议规定值为 0
     * 时按 4 处理；再加 1 才包含 boot sector。所以 setup_size 同时也是
     * 压缩内核载荷在文件中的起始偏移。
     */
    size_t setup_sectors = hdr.setup_sects ? hdr.setup_sects : 4;
    size_t setup_size = (setup_sectors + 1) * 512;

    /*
     * init_size（单位：字节）：内核在能够读取 e820 内存表前，从
     * runtime_start 开始所需的连续内存长度。它不是启动 Linux 所需的
     * 总内存大小，而是内核早期初始化阶段必须保证可用的内存窗口大小。
     */
    uint32_t init_size = hdr.init_size;

    /*
     * pref_address（客户机物理地址）：内核希望最终运行的位置。不可重定位
     * 内核必须在这里运行；可重定位内核在当前载入地址低于该地址时，也会
     * 把自己移动到这里。
     */
    uint64_t preferred = hdr.pref_address;

    /*
     * initrd_addr_max：内核能访问的 initramfs 最高字节地址。它是内核告诉
     * 加载器的限制，加载器只读取并遵守，不应改写。
     */
    uint32_t initrd_addr_max = hdr.initrd_addr_max;

    /*
     * kernel_alignment（单位：字节）：可重定位内核运行地址必须满足的
     * 对齐值，例如 0x200000 表示按 2 MiB 对齐。协议要求它是 2 的幂，
     * 后面据此把 runtime_start 向上取整。
     */
    uint32_t kernel_alignment = hdr.kernel_alignment;
    int relocatable = hdr.relocatable_kernel != 0;

    /*
     * init_size 从“内核最终运行地址”开始计算，而不是固定从 1 MiB 计算。
     * 可重定位内核会选择 max(载入地址, 首选地址)，然后向上对齐；不可
     * 重定位内核必须使用镜像指定的 preferred 地址。后续据此分配客户机内存。
     */
    uint64_t runtime_start = relocatable ?
        (preferred > KERNEL_ADDR ? preferred : KERNEL_ADDR) : preferred;
    if (relocatable && preferred <= UINT32_MAX && kernel_alignment &&
        !(kernel_alignment & (kernel_alignment - 1)))
        runtime_start = align_up(runtime_start, kernel_alignment);

    /*
     * 只接受本阶段支持的镜像：带 HdrS 的现代 bzImage、启动协议至少 2.12、
     * 使用高地址载入、启动头长到包含 init_size，并且 setup、压缩载荷及
     * 初始化内存窗口均在合法范围内。
     * kernel_alignment 必须是 2 的幂，才能交给 align_up() 计算。
     */
    if (memcmp(&hdr.header, "HdrS", 4) != 0 ||
        hdr.version < BOOT_PROTOCOL_MIN || !(hdr.loadflags & LOADED_HIGH) ||
        setup_header_end(&hdr) < SETUP_HEADER_MIN_END ||
        setup_size > 0x10000 || setup_size >= size ||
        init_size == 0 || init_size > BZIMAGE_MAX_SIZE ||
        init_size < size - setup_size || preferred > UINT32_MAX ||
        (relocatable && (!kernel_alignment ||
                           (kernel_alignment & (kernel_alignment - 1)))) ||
        runtime_start + init_size > UINT32_MAX) {
        fprintf(stderr, "unsupported or malformed x86 bzImage\n");
        free(data);
        return -1;
    }

    /* 保存解析结果；image->data 的所有权转交给调用者。 */
    image->data = data;
    image->size = size;
    image->setup_size = setup_size;
    image->kernel_size = size - setup_size;
    image->init_size = init_size;
    image->runtime_start = runtime_start;
    image->initrd_addr_max = initrd_addr_max;
    return 0;
}

/*
 * initramfs 默认放在 INITRD_ADDR（96 MiB）。如果内核最终运行窗口
 * [runtime_start, runtime_start + init_size) 延伸到了 96 MiB 之后，就顺延到
 * 窗口末尾向上按 2 MiB 对齐的位置，保证解压内核时不会覆盖 initramfs。
 */
uint64_t boot_linux_initrd_addr(const struct linux_image *image)
{
    uint64_t kernel_end = align_up(image->runtime_start + image->init_size,
                                   2u * 1024u * 1024u);
    return kernel_end > INITRD_ADDR ? kernel_end : INITRD_ADDR;
}

size_t boot_linux_memory_size(const struct linux_image *image)
{
    if (SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED)
        return BLOG_GUEST_MEMORY_SIZE;

    size_t size = (size_t)image->runtime_start + image->init_size +
                  32u * 1024u * 1024u;
    /* 总是为最大尺寸的 initramfs 留出位置，再多留 16 MiB 给内核使用。 */
    size_t initrd_end = (size_t)boot_linux_initrd_addr(image) + INITRD_MAX_SIZE +
                        16u * 1024u * 1024u;
    if (initrd_end > size)
        size = initrd_end;
    const size_t memory_alignment = 2u * 1024u * 1024u;
    size = (size_t)align_up(size, memory_alignment);
    return size < GUEST_MEMORY_MIN_SIZE ? GUEST_MEMORY_MIN_SIZE : size;
}

/*
 * 按 Linux x86 boot protocol 把已经解析过的 bzImage 放入客户机内存。
 * 本阶段不执行 16 位 setup 代码，而是直接以 32 位保护模式跳到
 * KERNEL_ADDR，并让 RSI 指向 BOOT_PARAMS_ADDR。
 *
 * 客户机物理内存布局（地址从低到高）：
 *
 *   e820 [0, 64 KiB) 保留 —— 没有 BIOS，这里只放 VMM 写入的数据
 *     0x00000500  临时 GDT（4 项，32 字节）  BOOT_GDT_ADDR，GDTR 指向这里
 *     0x00009000  boot_params（4 KiB）       BOOT_PARAMS_ADDR，RSI 指向这里；
 *                   +0x1f1 setup_header      e820 表就在 boot_params 内部，
 *                   +0x2d0 e820_table        不是单独的一块内存
 *
 *   e820 [64 KiB, 1 MiB) 可用
 *     0x00010000  setup 代码副本             SETUP_CODE_ADDR，只供检查，不执行
 *     0x00020000  内核命令行（≤ 4 KiB）      CMDLINE_ADDR
 *     0x00090000  入口临时栈顶，向下增长     BOOT_STACK_ADDR，RSP 初值
 *     0x000e0000  ACPI 表                    内核把 640K–1M 视为 BIOS 区并保留
 *
 *   e820 [1 MiB, memory->size) 可用
 *     0x00100000  压缩内核载荷               KERNEL_ADDR，RIP 从这里开始
 *     runtime_start  内核运行窗口            内核把自己解压到这里，长度 init_size；
 *                                            地址由启动头算出，常见为 16 MiB
 *     initrd 地址    initramfs（≤ 144 MiB）  max(96 MiB, 窗口末尾按 2 MiB 对齐)，
 *                                            见 boot_linux_initrd_addr()
 *     memory->size   客户机内存末尾          至少 256 MiB
 */
int boot_linux_prepare(struct guest_memory *memory, const struct linux_image *image,
                       const char *cmdline, const struct linux_initrd *initrd)
{
    /*
     * 在进行任何复制前验证内核最终运行窗口、压缩载荷、setup 副本，
     * 以及 setup 与命令行之间的边界。减法形式的检查可以避免
     * address + size 本身发生整数溢出。
     */
    if (!memory->data || !image->data || memory->size < KERNEL_ADDR ||
        image->runtime_start > memory->size ||
        image->init_size > memory->size - image->runtime_start ||
        image->kernel_size > memory->size - KERNEL_ADDR ||
        image->setup_size > CMDLINE_ADDR - SETUP_CODE_ADDR) {
        errno = EINVAL;
        return -1;
    }
    /*
     * initramfs 必须完整落在客户机内存里、不能与内核运行窗口重叠，
     * 最后一个字节不能超过内核声明的 initrd_addr_max，且 ramdisk_image /
     * ramdisk_size 都只有 32 位。
     */
    if (initrd && initrd->size) {
        uint64_t kernel_end = image->runtime_start + image->init_size;
        uint64_t initrd_last = initrd->addr + initrd->size - 1;
        if (initrd->size > INITRD_MAX_SIZE || initrd->addr < kernel_end ||
            initrd->addr > memory->size ||
            initrd->size > memory->size - initrd->addr ||
            initrd_last > image->initrd_addr_max || initrd_last > UINT32_MAX) {
            errno = EINVAL;
            return -1;
        }
    }

    /*
     * cmdline_size 来自镜像启动头；本实现还设置了 4096 字节的本地上限。
     * 两个上限都不包含最后写入客户机内存的 NUL。no_cmdline 消融项不会
     * 使用命令行，因此也不应该因为宿主机传入的字符串过长而提前失败。
     */
    size_t cmdline_len = strlen(cmdline);
    const struct setup_header *source_hdr =
        (const struct setup_header *)(image->data + SETUP_HEADER_OFFSET);
    if (!SVMM_ABLATE_CMDLINE_ENABLED &&
        (cmdline_len >= CMDLINE_MAX_LEN ||
         (source_hdr->cmdline_size && cmdline_len > source_hdr->cmdline_size))) {
        errno = E2BIG;
        return -1;
    }

    /*
     * zero page 必须先清零，再只复制镜像声明存在的 setup_header 字节。
     * 这样旧协议中不存在的尾部字段保持为 0，也不会越过本地结构体。
     * 启动头长度由镜像自己的 jump 指令决定（见 setup_header_end()），
     * 最后仍用 sizeof(params.hdr) 限制复制范围。
     */
    struct boot_params params = { 0 };
    size_t header_size = setup_header_end(source_hdr) - SETUP_HEADER_OFFSET;
    if (header_size > sizeof(params.hdr))
        header_size = sizeof(params.hdr);
    memcpy(&params.hdr, source_hdr, header_size);

    /*
     * 0xff 表示未登记的引导器。CAN_USE_HEAP/heap_end_ptr 告诉早期 setup
     * 代码低端内存中可用堆的末端；code32_start 则明确 32 位入口为 1 MiB。
     * 当前 VMM 直接进入 code32_start，但仍把这些字段填写完整，保证
     * zero page 与 Linux boot protocol 一致。
     */
    params.hdr.type_of_loader = 0xff;
    params.hdr.loadflags |= CAN_USE_HEAP;
    params.hdr.heap_end_ptr = 0xde00;
    params.hdr.cmd_line_ptr = SVMM_ABLATE_CMDLINE_ENABLED ? 0 : CMDLINE_ADDR;
    params.hdr.code32_start = KERNEL_ADDR;
    /*
     * 内核看到 ramdisk_image 非零，就会在 populate_rootfs() 中把这段 cpio
     * （可带 gzip 压缩）解包到 rootfs，然后执行其中的 /init。
     */
    if (initrd && initrd->size) {
        params.hdr.ramdisk_image = (uint32_t)initrd->addr;
        params.hdr.ramdisk_size = (uint32_t)initrd->size;
    }
    /*
     * 告诉内核哪些客户机物理地址可以分配：最低 64 KiB 保留，
     * 64 KiB 到 1 MiB 可用，1 MiB 以上一直覆盖实际分配的客户机内存。
     * no_e820 只把条目数设为 0，供消融实验观察内核的失败位置。
     */
    params.e820_entries = SVMM_ABLATE_E820_ENABLED ? 0 : 3;
    if (!SVMM_ABLATE_E820_ENABLED) {
        params.e820_table[0] = (struct boot_e820_entry){
            .addr = 0, .size = 0x10000, .type = E820_RESERVED,
        };
        params.e820_table[1] = (struct boot_e820_entry){
            .addr = 0x10000, .size = 0xf0000, .type = E820_RAM,
        };
        params.e820_table[2] = (struct boot_e820_entry){
            .addr = 0x100000, .size = memory->size - 0x100000, .type = E820_RAM,
        };
    }
    /*
     * bzImage 文件在 setup_size 处分成两部分。后半段压缩内核放到 1 MiB
     * 并作为 vCPU 的入口；前半段保留在 64 KiB，便于内核和调试代码检查
     * 原始 setup 区，但当前启动路径不会从那里执行。
     */
    if (guest_memory_load(memory, KERNEL_ADDR, image->data + image->setup_size,
                          image->kernel_size) < 0 ||
        guest_memory_load(memory, SETUP_CODE_ADDR, image->data,
                          image->setup_size) < 0)
        return -1;
    /* 命令行包含结尾 NUL；cmd_line_ptr 在前面已指向同一个地址。 */
    if (!SVMM_ABLATE_CMDLINE_ENABLED &&
        guest_memory_load(memory, CMDLINE_ADDR, cmdline, cmdline_len + 1) < 0)
        return -1;

    /*
     * vCPU 进入内核时 RSI 指向这里。no_boot_params 消融项保留其他载入
     * 步骤，只跳过 zero page，从而一次只移除一个启动组件。
     */
    if (!SVMM_ABLATE_BOOT_PARAMS_ENABLED &&
        guest_memory_load(memory, BOOT_PARAMS_ADDR, &params, sizeof(params)) < 0)
        return -1;

    /*
     * 保护模式下，CS/DS/SS 保存的是 GDT selector，而不是段基址。
     * selector 除以 8 就是 GDT 下标：vcpu_setup_linux_boot() 把 CS 设为
     * BOOT_CS_SELECTOR（0x10，index 2），数据段设为 BOOT_DS_SELECTOR
     * （0x18，index 3）。两个描述符的 base 都是 0，limit 都覆盖 4 GiB：
     *
     *   index 0  0x0000000000000000  空描述符（CPU 要求第 0 项为空）
     *   index 1  0x0000000000000000  未使用
     *   index 2  0x00cf9b000000ffff  32 位可执行代码段
     *   index 3  0x00cf93000000ffff  32 位可写数据段
     *
     * 描述符各位的含义与 vcpu.c 中 struct kvm_segment 的字段一一对应：
     * 0x9b/0x93 是 present=1、dpl=0、s=1 加 type=0xb/0x3；0xc 是 g=1、db=1。
     */
    uint64_t gdt[BOOT_GDT_ENTRIES] = { 0 };
    gdt[BOOT_CS_SELECTOR / 8] = UINT64_C(0x00cf9b000000ffff);
    gdt[BOOT_DS_SELECTOR / 8] = UINT64_C(0x00cf93000000ffff);
    if (guest_memory_load(memory, BOOT_GDT_ADDR, gdt, sizeof(gdt)) < 0)
        return -1;

    /*
     * params.hdr 已被本函数修改，所以同步更新 64 KiB 处 setup 副本中的
     * 启动头。zero page 和 setup 副本由此不会显示互相矛盾的入口、堆和
     * 命令行信息；即使 no_boot_params 跳过 zero page，该副本仍可供检查。
     */
    memcpy(memory->data + SETUP_CODE_ADDR + SETUP_HEADER_OFFSET,
           &params.hdr, sizeof(params.hdr));
    return 0;
}

/*
 * 把 initramfs 文件（通常是 cpio.gz）直接读进客户机物理地址 address，
 * 成功后 *size 为文件字节数，供 boot_linux_prepare() 写入 ramdisk_size。
 */
int boot_linux_load_initramfs(struct guest_memory *memory, uint64_t address,
                              const char *path, size_t *size)
{
    *size = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open initramfs");
        return -1;
    }
    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0 || !S_ISREG(statbuf.st_mode) ||
        statbuf.st_size <= 0 || (uint64_t)statbuf.st_size > INITRD_MAX_SIZE ||
        address > memory->size ||
        (uint64_t)statbuf.st_size > memory->size - address) {
        fprintf(stderr, "invalid initramfs size or location\n");
        close(fd);
        return -1;
    }
    size_t file_size = (size_t)statbuf.st_size;
    size_t offset = 0;
    while (offset < file_size) {
        ssize_t n = read(fd, memory->data + address + offset, file_size - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                fprintf(stderr, "read initramfs: unexpected end of file\n");
            else
                perror("read initramfs");
            close(fd);
            return -1;
        }
        offset += (size_t)n;
    }
    close(fd);
    *size = file_size;
    return 0;
}

void boot_linux_free(struct linux_image *image)
{
    free(image->data);
    *image = (struct linux_image){ 0 };
}

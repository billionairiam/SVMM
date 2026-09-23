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

_Static_assert(offsetof(struct boot_params, hdr) == 0x1f1,
               "unexpected Linux boot header layout");
_Static_assert(offsetof(struct boot_params, e820_table) == 0x2d0,
               "unexpected Linux e820 layout");

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint64_t read_le64(const uint8_t *data)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= (uint64_t)data[i] << (8 * i);
    return value;
}

/* alignment 必须是 2 的幂。 */
static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
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
     * 在分配内存前检查文件大小：0x264 是当前会读取到的最后一个
     * 启动头字段 init_size 的末尾，512 MiB 是本程序设置的安全上限。
     */
    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0 || statbuf.st_size < 0 ||
        statbuf.st_size > BZIMAGE_MAX_SIZE || statbuf.st_size < 0x264) {
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
     * setup_sects 位于文件偏移 0x1f1，记录 boot sector 后面还有多少个
     * setup 扇区。协议规定值为 0 时按 4 处理；再加 1 才包含 boot sector。
     * 所以 setup_size 同时也是压缩内核载荷在文件中的起始偏移。
     */
    size_t setup_sectors = data[0x1f1] ? data[0x1f1] : 4;
    size_t setup_size = (setup_sectors + 1) * 512;

    /* 下面这些偏移都来自 Linux x86 boot protocol 的 setup_header。 */
    uint16_t version = read_le16(data + 0x206);

    /*
     * init_size（偏移 0x260，单位：字节）：内核在能够读取 e820 内存表前，
     * 从 runtime_start 开始所需的连续内存长度。它不是启动 Linux 所需的
     * 总内存大小，而是内核早期初始化阶段必须保证可用的内存窗口大小。
     */
    uint32_t init_size;
    memcpy(&init_size, data + 0x260, sizeof(init_size));

    /*
     * preferred（pref_address，偏移 0x258，单位：客户机物理地址）：内核
     * 希望最终运行的位置。不可重定位内核必须在这里运行；可重定位内核在
     * 当前载入地址低于该地址时，也会把自己移动到这里。
     */
    uint64_t preferred = read_le64(data + 0x258);

    /*
     * kernel_alignment（偏移 0x230，单位：字节）：可重定位
     * 内核运行地址必须满足的对齐值，例如 0x200000 表示按 2 MiB 对齐。
     * 协议要求它是 2 的幂，后面据此把 runtime_start 向上取整。
     */
    uint32_t kernel_alignment;
    memcpy(&kernel_alignment, data + 0x230, sizeof(kernel_alignment));
    int relocatable = data[0x234] != 0;

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
     * 使用高地址载入，并且 setup、压缩载荷及初始化内存窗口均在合法范围内。
     * kernel_alignment 必须是 2 的幂，才能交给 align_up() 计算。
     */
    if (memcmp(data + 0x202, "HdrS", 4) != 0 ||
        version < BOOT_PROTOCOL_MIN || !(data[0x211] & LOADED_HIGH) ||
        data[0x201] < 0x62 || setup_size > 0x10000 || setup_size >= size ||
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
    return 0;
}

size_t boot_linux_memory_size(const struct linux_image *image)
{
    if (SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED)
        return BLOG_GUEST_MEMORY_SIZE;

    size_t size = (size_t)image->runtime_start + image->init_size +
                  32u * 1024u * 1024u;
    const size_t memory_alignment = 2u * 1024u * 1024u;
    size = (size_t)align_up(size, memory_alignment);
    return size < GUEST_MEMORY_MIN_SIZE ? GUEST_MEMORY_MIN_SIZE : size;
}

/*
 * 按 Linux x86 boot protocol 把已经解析过的 bzImage 放入客户机内存。
 * 本阶段不执行 16 位 setup 代码，而是直接以 32 位保护模式跳到
 * KERNEL_ADDR，并让 RSI 指向 BOOT_PARAMS_ADDR。
 *
 * 主要客户机物理地址布局：
 *
 *   0x00000500  GDT
 *   0x00009000  struct boot_params（zero page）
 *   0x00010000  bzImage 的 boot sector 和 setup 代码副本
 *   0x00020000  以 NUL 结尾的内核命令行
 *   0x00100000  压缩内核载荷，即 32 位入口
 *
 * boot_params 中本函数关心的协议偏移：
 *
 *   0x1e8  e820_entries           e820 条目数
 *   0x1f1  hdr.setup_sects        setup 扇区数
 *   0x210  hdr.type_of_loader     引导加载器 ID
 *   0x211  hdr.loadflags          CAN_USE_HEAP 等标志
 *   0x228  hdr.cmd_line_ptr       命令行客户机物理地址
 *   0x230  hdr.kernel_alignment   可重定位内核的对齐要求
 *   0x234  hdr.relocatable_kernel 可重定位标志
 *   0x238  hdr.cmdline_size       镜像允许的最大命令行长度
 *   0x258  hdr.pref_address       内核首选运行地址
 *   0x260  hdr.init_size          早期初始化内存窗口大小
 *   0x2d0  e820_table             BIOS 风格物理内存表
 */
int boot_linux_prepare(struct guest_memory *memory, const struct linux_image *image,
                       const char *cmdline)
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
     * cmdline_size 来自镜像启动头；本实现还设置了 4096 字节的本地上限。
     * 两个上限都不包含最后写入客户机内存的 NUL。no_cmdline 消融项不会
     * 使用命令行，因此也不应该因为宿主机传入的字符串过长而提前失败。
     */
    size_t cmdline_len = strlen(cmdline);
    const struct setup_header *source_hdr =
        (const struct setup_header *)(image->data + 0x1f1);
    if (!SVMM_ABLATE_CMDLINE_ENABLED &&
        (cmdline_len >= CMDLINE_MAX_LEN ||
         (source_hdr->cmdline_size && cmdline_len > source_hdr->cmdline_size))) {
        errno = E2BIG;
        return -1;
    }

    /*
     * zero page 必须先清零，再只复制镜像声明存在的 setup_header 字节。
     * 这样旧协议中不存在的尾部字段保持为 0，也不会越过本地结构体。
     *
     * 文件偏移 0x200 是一条两字节的短跳转指令 `EB xx`，0x201 的 xx
     * 是以 0x202（该指令结束后的地址）为基准的 8 位相对位移。合法
     * bzImage 在这里向前跳到 setup 代码入口，也就是实际启动头的末尾。因此：
     *
     *   启动头末尾 = 0x202 + image->data[0x201]
     *   启动头长度 = 启动头末尾 - setup_header 起点 0x1f1
     *
     * 这个长度来自镜像本身，最后仍用 sizeof(params.hdr) 限制复制范围。
     */
    struct boot_params params = { 0 };
    size_t header_size = image->data[0x201] + 0x202 - 0x1f1;
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
     * vcpu_setup_linux_boot() 会把 CS 设为 0x10（GDT index 2），把数据段
     * 设为 0x18（index 3）。两个描述符的 base 都是 0，limit 都覆盖 4 GiB：
     *
     *   index 0  0x0000000000000000  空描述符
     *   index 1  0x0000000000000000  保留
     *   index 2  0x00cf9b000000ffff  32 位可执行代码段
     *   index 3  0x00cf93000000ffff  32 位可写数据段
     */
    const uint64_t gdt[4] = {
        0,
        0,
        UINT64_C(0x00cf9b000000ffff),
        UINT64_C(0x00cf93000000ffff),
    };
    if (guest_memory_load(memory, 0x500, gdt, sizeof(gdt)) < 0)
        return -1;

    /*
     * params.hdr 已被本函数修改，所以同步更新 64 KiB 处 setup 副本中的
     * 启动头。zero page 和 setup 副本由此不会显示互相矛盾的入口、堆和
     * 命令行信息；即使 no_boot_params 跳过 zero page，该副本仍可供检查。
     */
    memcpy(memory->data + SETUP_CODE_ADDR + 0x1f1,
           &params.hdr, sizeof(params.hdr));
    return 0;
}

void boot_linux_free(struct linux_image *image)
{
    free(image->data);
    *image = (struct linux_image){ 0 };
}

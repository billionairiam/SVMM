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

/* 拒绝异常巨大的文件，避免一次 malloc 过多宿主机内存。 */
#define BZIMAGE_MAX_SIZE (512u * 1024u * 1024u)
/*
 * 启动协议 2.12 对应 Linux 3.8（2013 年）。本加载器读取的 pref_address、
 * init_size（2.10 引入）等字段在这个版本之后都一定存在。
 */
#define BOOT_PROTOCOL_MIN 0x020cu
/*
 * e820 条目类型（名字来自 BIOS 调用 int 0x15, AX=0xe820）：
 *   1 = 可用 RAM，2 = 保留（固件/设备占用，操作系统不能用），
 *   3 = ACPI 可回收，4 = ACPI NVS，5 = 坏内存。这里只用到前两种。
 */
#define E820_RAM 1u
#define E820_RESERVED 2u

/*
 * struct boot_params 来自内核头文件 <asm/bootparam.h>，它就是 4 KiB 的
 * “零页”（zero page）。bzImage 里的 setup_header 正好要放在零页的 0x1f1
 * 偏移处，这两个断言确保宿主机头文件与协议文档的偏移一致。
 */
_Static_assert(offsetof(struct boot_params, hdr) == 0x1f1,
               "unexpected Linux boot header layout");
_Static_assert(offsetof(struct boot_params, e820_table) == 0x2d0,
               "unexpected Linux e820 layout");

/* x86 是小端序：低字节在前。按字节拼接可避免未对齐访问。 */
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
 * bzImage 的文件布局（偏移均为文件内字节偏移）：
 *
 *   0x000  "MZ"                   开启 EFI stub 时的 DOS/PE 头，供 UEFI 识别；
 *                                 BIOS 时代这里是软盘引导扇区代码
 *   0x03c  PE 头偏移              UEFI 据此找到 "PE\0\0" 头（本 VMM 不使用）
 *   0x1f1  setup_header 开始      引导器和内核之间的“合同”，见下表
 *   0x1fe  0xAA55                 传统引导扇区签名 boot_flag
 *   0x200  EB xx                  短跳转指令，跳过启动头进入 16 位 setup 代码
 *   0x202  "HdrS"                 现代启动头的魔数
 *   ...    16 位 setup 代码       依赖 BIOS 中断，本 VMM 不执行
 *   setup_size = (setup_sects + 1) * 512
 *          保护模式部分开始       开头是解压器入口 startup_32，后接压缩内核
 *   文件末尾
 *
 * 本文件用到的 setup_header 字段（偏移相对文件开头，也等于在零页中的
 * 偏移，因为零页的 0x1f1 处正好放一份 setup_header）：
 *
 *   偏移   字段                 谁填写  含义
 *   0x1f1  setup_sects          内核    setup 部分的扇区数（不含引导扇区）
 *   0x202  header               内核    魔数 "HdrS"
 *   0x206  version              内核    启动协议版本，0x020c 即 2.12
 *   0x210  type_of_loader       引导器  引导器 ID，0xff 表示“未登记”
 *   0x211  loadflags            双方    bit 0 LOADED_HIGH：保护模式部分在 1 MiB；
 *                                       bit 7 CAN_USE_HEAP：heap_end_ptr 有效
 *   0x214  code32_start         引导器  32 位入口地址
 *   0x224  heap_end_ptr         引导器  16 位 setup 代码可用的堆/栈末尾
 *   0x228  cmd_line_ptr         引导器  命令行字符串的客户机物理地址
 *   0x230  kernel_alignment     内核    可重定位内核运行地址的对齐要求
 *   0x234  relocatable_kernel   内核    非 0 表示内核可以在其他地址运行
 *   0x238  cmdline_size         内核    命令行最大长度（不含结尾 NUL）
 *   0x258  pref_address         内核    内核首选的运行地址
 *   0x260  init_size            内核    解压和早期初始化需要的内存大小
 *
 * 注意：教程中的偏移表把 relocatable_kernel 写成 0x238、cmdline_size
 * 写成 0x23c，这是错的，正确值以内核文档 Documentation/arch/x86/boot.rst
 * 和 <asm/bootparam.h> 为准（上面的 _Static_assert 也会帮忙核对）。
 *
 * 零页里还有两个不属于 setup_header、由引导器填写的字段：
 *   0x1e8  e820_entries         e820 表的条目数
 *   0x2d0  e820_table           物理内存表，见 boot_linux_prepare()
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
     * 启动头字段 init_size（0x260 起 4 字节）的末尾，文件至少要这么长，
     * 后面按偏移读字段才不会越界；512 MiB 是本程序设置的安全上限。
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

    /*
     * 下面这些偏移都来自 Linux x86 boot protocol 的 setup_header。
     * version 的高字节是主版本、低字节是次版本：0x020c 表示 2.12。
     */
    uint16_t version = read_le16(data + 0x206);

    /*
     * init_size（偏移 0x260，单位：字节）：内核在能够读取 e820 内存表前，
     * 从 runtime_start 开始所需的连续内存长度。它不是启动 Linux 所需的
     * 总内存大小，而是内核早期初始化阶段必须保证可用的内存窗口大小。
     * 解压器要把压缩内核解压到这个窗口里，所以它通常比 bzImage 文件
     * 本身大好几倍（发行版内核常见几十 MiB）。
     */
    uint32_t init_size;
    memcpy(&init_size, data + 0x260, sizeof(init_size));

    /*
     * preferred（pref_address，偏移 0x258，单位：客户机物理地址）：内核
     * 希望最终运行的位置。不可重定位内核必须在这里运行；可重定位内核在
     * 当前载入地址低于该地址时，也会把自己移动到这里。x86_64 默认是
     * 16 MiB（CONFIG_PHYSICAL_START=0x1000000）。我们把压缩内核放在 1 MiB，
     * 解压器会把解压结果放到这个更高的地址，而不是原地解压。
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
     * 只接受本阶段支持的镜像，各条件依次为：
     *   - 0x202 处是 "HdrS"：否则是很老的 zImage 或根本不是内核；
     *   - 协议版本 >= 2.12：保证后面读取的字段都存在；
     *   - LOADED_HIGH：保护模式部分要求放在 1 MiB（bzImage 都是如此，
     *     老式 zImage 放在 0x10000，本 VMM 不支持）；
     *   - data[0x201] >= 0x62：0x200 处跳转指令的位移决定启动头有多长，
     *     0x202 + 0x62 = 0x264 正好是 init_size 的末尾，说明头里确实包含
     *     我们读到的所有字段；
     *   - setup 部分不超过 64 KiB：它要放在 0x10000 且不能覆盖 0x20000 的
     *     命令行；并且后面必须还有保护模式部分；
     *   - init_size 非 0、不离谱，且至少容纳得下保护模式部分本身；
     *   - 运行地址和 init_size 窗口都在 4 GiB 以内：32 位入口时未开分页，
     *     只能访问 32 位物理地址；
     *   - kernel_alignment 必须是 2 的幂，才能交给 align_up() 计算。
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

/*
 * 计算客户机内存大小：内核运行窗口末尾（runtime_start + init_size）之后
 * 再多给 32 MiB，供内核解压完成后的页分配器、页表、slab 等使用；按 2 MiB
 * 对齐，方便宿主机用大页映射；最少 128 MiB。
 *
 * 教程固定使用 32 MiB。但较新的内核 pref_address 就在 16 MiB，init_size
 * 又有几十 MiB，32 MiB 会装不下（fixed_32m 消融实验验证了这一点）。
 */
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
 * 这个函数做的就是 GRUB（或 QEMU -kernel）在跳进内核前做的事：
 *   1. 构造 boot_params（零页）：机器有多少内存、命令行在哪里等；
 *   2. 写入 e820 内存表：真实机器上由 BIOS int 0x15 或 UEFI 内存表提供；
 *   3. 复制内核和命令行到约定地址；
 *   4. 写一张最小 GDT：32 位协议要求的平坦代码段和数据段。
 *
 * 客户机物理地址布局（e820 表直接写在 boot_params 内部，没有单独的副本）：
 *
 *   +-------------------------------+ 0x00000000
 *   | IVT + BDA，e820 中标为保留     |
 *   +-------------------------------+ 0x00000500
 *   | GDT（null、null、CS、DS）       |  GDT_ADDR
 *   +-------------------------------+ 0x00009000
 *   | struct boot_params（零页）     |  BOOT_PARAMS_ADDR，RSI 指向这里
 *   +-------------------------------+ 0x00010000
 *   | boot sector + setup 代码副本   |  SETUP_CODE_ADDR，只作数据，不执行
 *   +-------------------------------+ 0x00020000
 *   | 以 NUL 结尾的内核命令行        |  CMDLINE_ADDR
 *   +-------------------------------+ 0x00090000
 *   | 32 位入口使用的临时栈（向下长）|  见 vcpu_setup_linux_boot()
 *   +-------------------------------+ 0x00100000
 *   | 压缩内核载荷，即 32 位入口      |  KERNEL_ADDR
 *   +-------------------------------+ boot_linux_memory_size()
 *   客户机 RAM 结束：至少 128 MiB，并覆盖内核解压后的运行窗口
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
     * 为什么叫“零页”：引导器先把一整页（4 KiB）清零，再只填自己知道的
     * 字段。内核把 0 理解为“没有提供”，例如 efi_info 为 0 就说明不是
     * 从 UEFI 启动，acpi_rsdp_addr 为 0 就自己去找 ACPI 表。
     *
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
     * 复制完内核写好的字段后，再填写“引导器负责”的字段：
     *   type_of_loader = 0xff  未在内核登记过 ID 的引导器（GRUB 是 0x7）；
     *   CAN_USE_HEAP + heap_end_ptr = 0xde00
     *                          16 位 setup 代码的堆/栈到 setup 段起点
     *                          + 0xde00 + 0x200 = 0x1e000 为止，不会碰到
     *                          0x20000 处的命令行；
     *   cmd_line_ptr           命令行字符串的物理地址，内核从这里读参数；
     *   code32_start           32 位入口，即压缩内核放置的 1 MiB。
     * 当前 VMM 直接进入 code32_start、不执行 setup 代码，堆字段实际上
     * 用不到，但仍把它们填写完整，保证 zero page 与 Linux boot protocol 一致。
     */
    params.hdr.type_of_loader = 0xff;
    params.hdr.loadflags |= CAN_USE_HEAP;
    params.hdr.heap_end_ptr = 0xde00;
    params.hdr.cmd_line_ptr = SVMM_ABLATE_CMDLINE_ENABLED ? 0 : CMDLINE_ADDR;
    params.hdr.code32_start = KERNEL_ADDR;
    /*
     * e820 内存表告诉内核“哪些物理地址是可以随便用的 RAM”。真实机器上
     * 它来自 BIOS 的 int 0x15/0xe820，或由 EFI stub 把 UEFI 内存表转换而来；
     * 这里没有固件，由 VMM 根据自己 mmap 的客户机内存直接写出来：
     *
     *   [0x00000, 0x10000)        保留  中断向量表、BIOS 数据区、GDT、零页
     *   [0x10000, 0x100000)       RAM   setup 副本、命令行、临时栈都在这里
     *   [0x100000, memory->size)  RAM   压缩内核、解压后的内核和其余主内存
     *
     * 真实 PC 上 0xa0000–0x100000 是显存和 BIOS ROM，要标成保留；这个 VMM
     * 没有这些设备，那段地址也是普通 RAM，所以整段标为可用。
     *
     * setup 副本和命令行放在“可用 RAM”里没问题：启动协议允许引导器
     * 临时占用，内核会在用完（例如把命令行复制走）之后再回收这些页。
     * 如果把保留区错标为 RAM，内核可能覆盖关键数据；反过来则内核能用的
     * 内存变少，甚至无法启动。
     *
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
     * GDT（全局描述符表）是保护模式下的“段表”。实模式里段寄存器装的是
     * 段基址 / 16；保护模式里段寄存器装的是 selector，selector >> 3 是 GDT
     * 下标，CPU 从对应的 8 字节描述符里取出基址、界限和权限。
     *
     * vcpu_setup_linux_boot() 会把 CS 设为 0x10（GDT index 2），把数据段
     * 设为 0x18（index 3），这是 32 位启动协议规定的 __BOOT_CS/__BOOT_DS：
     *
     *   index 0  0x0000000000000000  空描述符（CPU 规定第 0 项不可用）
     *   index 1  0x0000000000000000  保留
     *   index 2  0x00cf9b000000ffff  32 位可执行代码段
     *   index 3  0x00cf93000000ffff  32 位可写数据段
     *
     * 以代码段 0x00cf9b000000ffff 为例，按位拆开（从低位到高位）：
     *
     *   bits  0–15  0xffff   limit[15:0]
     *   bits 16–39  0x000000 base[23:0]
     *   bits 40–47  0x9b     访问字节 = 1001 1011b：
     *                          P=1 段存在、DPL=00 内核态、S=1 代码/数据段、
     *                          type=1011 可执行、可读、已访问
     *   bits 48–51  0xf      limit[19:16]，与上面合起来 limit = 0xfffff
     *   bits 52–55  0xc      标志 = 1100b：G=1 界限以 4 KiB 为单位、
     *                          D/B=1 默认 32 位操作数
     *   bits 56–63  0x00     base[31:24]
     *
     * 所以 base = 0、limit = (0xfffff + 1) * 4 KiB = 4 GiB，即“平坦段”：
     * 段地址就等于线性地址。数据段只有访问字节不同：0x93 的 type=0011
     * 表示可读、可写、已访问的数据段。
     */
    const uint64_t gdt[4] = {
        0,
        0,
        UINT64_C(0x00cf9b000000ffff),
        UINT64_C(0x00cf93000000ffff),
    };
    if (guest_memory_load(memory, GDT_ADDR, gdt, sizeof(gdt)) < 0)
        return -1;

    /*
     * params.hdr 已被本函数修改，所以同步更新 64 KiB 处 setup 副本中的
     * 启动头（setup 副本的 0x1f1 偏移与零页的 0x1f1 偏移是同一个结构）。zero page 和 setup 副本由此不会显示互相矛盾的入口、堆和
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

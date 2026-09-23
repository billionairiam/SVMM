#include "boot/linux.h"

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
    uint32_t init_size;
    memcpy(&init_size, data + 0x260, sizeof(init_size));
    uint64_t preferred = read_le64(data + 0x258);
    uint32_t alignment;
    memcpy(&alignment, data + 0x230, sizeof(alignment));
    int relocatable = data[0x234] != 0;

    /*
     * init_size 从“内核最终运行地址”开始计算，而不是固定从 1 MiB 计算。
     * 可重定位内核会选择 max(载入地址, 首选地址)，然后向上对齐；不可
     * 重定位内核必须使用镜像指定的 preferred 地址。后续据此分配客户机内存。
     */
    uint64_t runtime_start = relocatable ?
        (preferred > KERNEL_ADDR ? preferred : KERNEL_ADDR) : preferred;
    if (relocatable && preferred <= UINT32_MAX && alignment &&
        !(alignment & (alignment - 1)))
        runtime_start = (runtime_start + alignment - 1) &
                        ~((uint64_t)alignment - 1);

    /*
     * 只接受本阶段支持的镜像：带 HdrS 的现代 bzImage、启动协议至少 2.12、
     * 使用高地址载入，并且 setup、压缩载荷及初始化内存窗口均在合法范围内。
     * alignment 必须是 2 的幂，才能使用上面的位运算完成向上对齐。
     */
    if (memcmp(data + 0x202, "HdrS", 4) != 0 ||
        version < BOOT_PROTOCOL_MIN || !(data[0x211] & LOADED_HIGH) ||
        data[0x201] < 0x62 || setup_size > 0x10000 || setup_size >= size ||
        init_size == 0 || init_size > BZIMAGE_MAX_SIZE ||
        init_size < size - setup_size || preferred > UINT32_MAX ||
        (relocatable && (!alignment || (alignment & (alignment - 1)))) ||
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
    size_t size = (size_t)image->runtime_start + image->init_size +
                  32u * 1024u * 1024u;
    const size_t alignment = 2u * 1024u * 1024u;
    size = (size + alignment - 1) & ~(alignment - 1);
    return size < GUEST_MEMORY_MIN_SIZE ? GUEST_MEMORY_MIN_SIZE : size;
}

int boot_linux_prepare(struct guest_memory *memory, const struct linux_image *image,
                       const char *cmdline)
{
    if (!memory->data || !image->data || memory->size < KERNEL_ADDR ||
        image->runtime_start > memory->size ||
        image->init_size > memory->size - image->runtime_start ||
        image->kernel_size > memory->size - KERNEL_ADDR ||
        image->setup_size > CMDLINE_ADDR - SETUP_CODE_ADDR) {
        errno = EINVAL;
        return -1;
    }
    size_t cmdline_len = strlen(cmdline);
    const struct setup_header *source_hdr =
        (const struct setup_header *)(image->data + 0x1f1);
    if (cmdline_len >= CMDLINE_MAX_LEN ||
        (source_hdr->cmdline_size && cmdline_len > source_hdr->cmdline_size)) {
        errno = E2BIG;
        return -1;
    }

    struct boot_params params = { 0 };
    size_t header_size = image->data[0x201] + 0x202 - 0x1f1;
    if (header_size > sizeof(params.hdr))
        header_size = sizeof(params.hdr);
    memcpy(&params.hdr, source_hdr, header_size);
    params.hdr.type_of_loader = 0xff;
    params.hdr.loadflags |= CAN_USE_HEAP;
    params.hdr.heap_end_ptr = 0xde00;
    params.hdr.cmd_line_ptr = CMDLINE_ADDR;
    params.hdr.code32_start = KERNEL_ADDR;
    params.e820_entries = 3;
    params.e820_table[0] = (struct boot_e820_entry){
        .addr = 0, .size = 0x10000, .type = E820_RESERVED,
    };
    params.e820_table[1] = (struct boot_e820_entry){
        .addr = 0x10000, .size = 0xf0000, .type = E820_RAM,
    };
    params.e820_table[2] = (struct boot_e820_entry){
        .addr = 0x100000, .size = memory->size - 0x100000, .type = E820_RAM,
    };
    if (guest_memory_load(memory, KERNEL_ADDR, image->data + image->setup_size,
                          image->kernel_size) < 0 ||
        guest_memory_load(memory, SETUP_CODE_ADDR, image->data,
                          image->setup_size) < 0 ||
        guest_memory_load(memory, CMDLINE_ADDR, cmdline, cmdline_len + 1) < 0 ||
        guest_memory_load(memory, BOOT_PARAMS_ADDR, &params, sizeof(params)) < 0)
        return -1;

    /* Selectors 0x10 and 0x18 required by the x86 32-bit boot protocol. */
    const uint64_t gdt[4] = {
        0,
        0,
        UINT64_C(0x00cf9b000000ffff),
        UINT64_C(0x00cf93000000ffff),
    };
    if (guest_memory_load(memory, 0x500, gdt, sizeof(gdt)) < 0)
        return -1;

    /* Keep the loaded real-mode setup header consistent for inspection. */
    memcpy(memory->data + SETUP_CODE_ADDR + 0x1f1,
           &params.hdr, sizeof(params.hdr));
    return 0;
}

void boot_linux_free(struct linux_image *image)
{
    free(image->data);
    *image = (struct linux_image){ 0 };
}

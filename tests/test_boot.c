#include "ablation.h"
#include "boot/linux.h"
#include "memory.h"

#include <asm/bootparam.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char fixture_path[] = "/tmp/svmm-bzimage-test-XXXXXX";

static void put16(uint8_t *data, size_t offset, uint16_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *data, size_t offset, uint32_t value)
{
    put16(data, offset, (uint16_t)value);
    put16(data, offset + 2, (uint16_t)(value >> 16));
}

static void put64(uint8_t *data, size_t offset, uint64_t value)
{
    put32(data, offset, (uint32_t)value);
    put32(data, offset + 4, (uint32_t)(value >> 32));
}

static void write_fixture(const uint8_t *data, size_t size)
{
    int fd = mkstemp(fixture_path);
    assert(fd >= 0);
    assert(write(fd, data, size) == (ssize_t)size);
    assert(close(fd) == 0);
}

static void reset_fixture_name(void)
{
    assert(unlink(fixture_path) == 0);
    strcpy(fixture_path, "/tmp/svmm-bzimage-test-XXXXXX");
}

static void write_temp(char *path, const void *data, size_t size)
{
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, data, size) == (ssize_t)size);
    assert(close(fd) == 0);
}

/*
 * 夹具内核运行窗口为 [2 MiB, 6 MiB)，客户机内存 8 MiB。检查：
 *   - 默认位置 96 MiB（小内核不需要顺延）；
 *   - 文件内容逐字节进入客户机内存，大小如实返回；
 *   - 空文件、放不进客户机内存的位置被拒绝；
 *   - ramdisk_image/ramdisk_size 写入 zero page；
 *   - 与内核窗口重叠、越过内存末尾、超过 initrd_addr_max 的位置被拒绝。
 */
static void check_initrd(struct guest_memory *memory, const struct linux_image *image)
{
    assert(boot_linux_initrd_addr(image) == INITRD_ADDR);

    const uint64_t addr = 6u * 1024u * 1024u;
    uint8_t cpio[4096];
    for (size_t i = 0; i < sizeof(cpio); ++i)
        cpio[i] = (uint8_t)(i * 7u + 1u);
    char path[] = "/tmp/svmm-initrd-test-XXXXXX";
    write_temp(path, cpio, sizeof(cpio));
    size_t size = 1;
    assert(boot_linux_load_initramfs(memory, addr, path, &size) == 0);
    assert(size == sizeof(cpio));
    assert(memcmp(memory->data + addr, cpio, sizeof(cpio)) == 0);
    assert(boot_linux_load_initramfs(memory, memory->size - 1024, path, &size) == -1);
    assert(size == 0);
    assert(unlink(path) == 0);

    char empty[] = "/tmp/svmm-initrd-empty-XXXXXX";
    write_temp(empty, "", 0);
    assert(boot_linux_load_initramfs(memory, addr, empty, &size) == -1);
    assert(unlink(empty) == 0);
    assert(boot_linux_load_initramfs(memory, addr, "/nonexistent/initrd", &size) == -1);

    struct linux_initrd initrd = { .addr = addr, .size = sizeof(cpio) };
    assert(boot_linux_prepare(memory, image, "console=ttyS0", &initrd) == 0);
#if !SVMM_ABLATE_BOOT_PARAMS_ENABLED
    const struct boot_params *params =
        (const struct boot_params *)(memory->data + BOOT_PARAMS_ADDR);
    assert(params->hdr.ramdisk_image == addr);
    assert(params->hdr.ramdisk_size == sizeof(cpio));
    assert(params->hdr.initrd_addr_max == 0x7fffffff);
#endif

    struct linux_initrd overlap = { .addr = 5u * 1024u * 1024u, .size = 4096 };
    errno = 0;
    assert(boot_linux_prepare(memory, image, "console=ttyS0", &overlap) == -1);
    assert(errno == EINVAL);

    struct linux_initrd past_end = { .addr = memory->size - 1024, .size = 4096 };
    assert(boot_linux_prepare(memory, image, "console=ttyS0", &past_end) == -1);

    struct linux_image low_limit = *image;
    low_limit.initrd_addr_max = (uint32_t)(addr + 2048);
    assert(boot_linux_prepare(memory, &low_limit, "console=ttyS0", &initrd) == -1);
}

int main(void)
{
    uint8_t file[1056] = { 0 };
    file[0x1f1] = 1;                 /* 1 setup sector + boot sector */
    file[0x201] = 0x62;              /* header ends after init_size */
    memcpy(file + 0x202, "HdrS", 4);
    put16(file, 0x206, 0x020c);
    file[0x211] = LOADED_HIGH;
    put32(file, 0x238, 255);        /* cmdline_size */
    put32(file, 0x230, 2 * 1024 * 1024);
    file[0x234] = 1;                /* relocatable_kernel */
    put64(file, 0x258, 0x100000);
    put32(file, 0x260, 4 * 1024 * 1024);
    put32(file, 0x22c, 0x7fffffff);  /* initrd_addr_max */
    file[0x300] = 0x5a;             /* setup code outside the header */
    memset(file + 1024, 0xa5, 32);

    write_fixture(file, sizeof(file));
    struct linux_image image = { 0 };
    assert(boot_linux_load(fixture_path, &image) == 0);
    assert(image.setup_size == 1024);
    assert(image.kernel_size == 32);
    assert(image.init_size == 4 * 1024 * 1024);
    assert(image.runtime_start == 2 * 1024 * 1024);

    struct guest_memory memory = {
        .data = calloc(1, 8 * 1024 * 1024),
        .size = 8 * 1024 * 1024,
    };
    assert(memory.data);
    assert(boot_linux_prepare(&memory, &image, "console=ttyS0", NULL) == 0);
    assert(memcmp(memory.data + KERNEL_ADDR, file + 1024, 32) == 0);
    assert(memory.data[SETUP_CODE_ADDR + 0x300] == 0x5a);
#if SVMM_ABLATE_CMDLINE_ENABLED
    assert(memory.data[CMDLINE_ADDR] == 0);
#else
    assert(strcmp((char *)memory.data + CMDLINE_ADDR, "console=ttyS0") == 0);
#endif

    const struct boot_params *params =
        (const struct boot_params *)(memory.data + BOOT_PARAMS_ADDR);
#if SVMM_ABLATE_BOOT_PARAMS_ENABLED
    assert(params->hdr.header == 0);
#else
    assert(params->hdr.version == 0x020c);
    assert(params->hdr.loadflags & CAN_USE_HEAP);
#if SVMM_ABLATE_CMDLINE_ENABLED
    assert(params->hdr.cmd_line_ptr == 0);
#else
    assert(params->hdr.cmd_line_ptr == CMDLINE_ADDR);
#endif
#if SVMM_ABLATE_E820_ENABLED
    assert(params->e820_entries == 0);
#else
    assert(params->e820_entries == 3);
    assert(params->e820_table[0].addr == 0);
    assert(params->e820_table[0].size == 0x10000);
    assert(params->e820_table[0].type == 2);
    assert(params->e820_table[1].addr == 0x10000);
    assert(params->e820_table[1].size == 0xf0000);
    assert(params->e820_table[1].type == 1);
    assert(params->e820_table[2].addr == 0x100000);
    assert(params->e820_table[2].size == memory.size - 0x100000);
    assert(params->e820_table[2].type == 1);
#endif
#endif
    const struct setup_header *setup_hdr =
        (const struct setup_header *)(memory.data + SETUP_CODE_ADDR + 0x1f1);
#if SVMM_ABLATE_CMDLINE_ENABLED
    assert(setup_hdr->cmd_line_ptr == 0);
#else
    assert(setup_hdr->cmd_line_ptr == CMDLINE_ADDR);
#endif
    assert(boot_linux_prepare(&memory, &image, "console=ttyS0 root=/dev/none", NULL) == 0);
#if SVMM_ABLATE_CMDLINE_ENABLED
    assert(memory.data[CMDLINE_ADDR] == 0);
#else
    assert(strcmp((char *)memory.data + CMDLINE_ADDR,
                  "console=ttyS0 root=/dev/none") == 0);
#endif
    check_initrd(&memory, &image);

    put64(file, 0x258, 256 * 1024 * 1024);
    struct linux_image high_image = { 0 };
    char high_fixture[] = "/tmp/svmm-bzimage-high-XXXXXX";
    int high_fd = mkstemp(high_fixture);
    assert(high_fd >= 0);
    assert(write(high_fd, file, sizeof(file)) == (ssize_t)sizeof(file));
    assert(close(high_fd) == 0);
    assert(boot_linux_load(high_fixture, &high_image) == 0);
    assert(high_image.runtime_start == 256 * 1024 * 1024);
    /* 内核窗口延伸到 96 MiB 之后时，initramfs 顺延到窗口末尾之后。 */
    assert(boot_linux_initrd_addr(&high_image) == 260 * 1024 * 1024);
#if SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED
    assert(boot_linux_memory_size(&high_image) == 32u * 1024u * 1024u);
    struct guest_memory blog_memory = {
        .data = calloc(1, 32u * 1024u * 1024u),
        .size = 32u * 1024u * 1024u,
    };
    assert(blog_memory.data);
    errno = 0;
    assert(boot_linux_prepare(&blog_memory, &high_image, "console=ttyS0", NULL) == -1);
    assert(errno == EINVAL);
    free(blog_memory.data);
#else
    assert(boot_linux_memory_size(&high_image) >= 260 * 1024 * 1024);
#endif
    assert(boot_linux_prepare(&memory, &high_image, "console=ttyS0", NULL) == -1);
    boot_linux_free(&high_image);
    assert(unlink(high_fixture) == 0);
    free(memory.data);
    boot_linux_free(&image);
    reset_fixture_name();

    file[0x202] = 'X';
    write_fixture(file, sizeof(file));
    assert(boot_linux_load(fixture_path, &image) == -1);
    reset_fixture_name();
    file[0x202] = 'H';

    put16(file, 0x206, 0x020b);
    write_fixture(file, sizeof(file));
    assert(boot_linux_load(fixture_path, &image) == -1);
    reset_fixture_name();

    put16(file, 0x206, 0x020c);
    file[0x211] = 0;
    write_fixture(file, sizeof(file));
    assert(boot_linux_load(fixture_path, &image) == -1);
    reset_fixture_name();
    return 0;
}

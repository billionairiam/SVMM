#include "boot/linux.h"
#include "memory.h"

#include <asm/bootparam.h>
#include <assert.h>
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
    assert(boot_linux_prepare(&memory, &image, "console=ttyS0") == 0);
    assert(memcmp(memory.data + KERNEL_ADDR, file + 1024, 32) == 0);
    assert(memory.data[SETUP_CODE_ADDR + 0x300] == 0x5a);
    assert(strcmp((char *)memory.data + CMDLINE_ADDR, "console=ttyS0") == 0);

    const struct boot_params *params =
        (const struct boot_params *)(memory.data + BOOT_PARAMS_ADDR);
    assert(params->hdr.version == 0x020c);
    assert(params->hdr.cmd_line_ptr == CMDLINE_ADDR);
    const struct setup_header *setup_hdr =
        (const struct setup_header *)(memory.data + SETUP_CODE_ADDR + 0x1f1);
    assert(setup_hdr->cmd_line_ptr == CMDLINE_ADDR);
    assert(params->hdr.loadflags & CAN_USE_HEAP);
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
    assert(boot_linux_prepare(&memory, &image, "console=ttyS0 root=/dev/none") == 0);
    assert(strcmp((char *)memory.data + CMDLINE_ADDR,
                  "console=ttyS0 root=/dev/none") == 0);
    put64(file, 0x258, 256 * 1024 * 1024);
    struct linux_image high_image = { 0 };
    char high_fixture[] = "/tmp/svmm-bzimage-high-XXXXXX";
    int high_fd = mkstemp(high_fixture);
    assert(high_fd >= 0);
    assert(write(high_fd, file, sizeof(file)) == (ssize_t)sizeof(file));
    assert(close(high_fd) == 0);
    assert(boot_linux_load(high_fixture, &high_image) == 0);
    assert(high_image.runtime_start == 256 * 1024 * 1024);
    assert(boot_linux_memory_size(&high_image) >= 260 * 1024 * 1024);
    assert(boot_linux_prepare(&memory, &high_image, "console=ttyS0") == -1);
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

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

int boot_linux_load(const char *path, struct linux_image *image)
{
    *image = (struct linux_image){ 0 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open bzImage");
        return -1;
    }
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

    size_t setup_sectors = data[0x1f1] ? data[0x1f1] : 4;
    size_t setup_size = (setup_sectors + 1) * 512;
    uint16_t version = read_le16(data + 0x206);
    uint32_t init_size;
    memcpy(&init_size, data + 0x260, sizeof(init_size));
    uint64_t preferred = read_le64(data + 0x258);
    uint32_t alignment;
    memcpy(&alignment, data + 0x230, sizeof(alignment));
    int relocatable = data[0x234] != 0;
    uint64_t runtime_start = relocatable ?
        (preferred > KERNEL_ADDR ? preferred : KERNEL_ADDR) : preferred;
    if (relocatable && preferred <= UINT32_MAX && alignment &&
        !(alignment & (alignment - 1)))
        runtime_start = (runtime_start + alignment - 1) &
                        ~((uint64_t)alignment - 1);
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

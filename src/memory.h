#ifndef GUEST_MEMORY_H
#define GUEST_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define GUEST_MEMORY_MIN_SIZE (128u * 1024u * 1024u)
#define KERNEL_ADDR 0x00100000u
#define SETUP_CODE_ADDR 0x00010000u
#define BOOT_PARAMS_ADDR 0x00009000u
#define CMDLINE_ADDR 0x00020000u
#define CMDLINE_MAX_LEN 4096u

struct guest_memory {
    uint8_t *data;
    size_t size;
};

int guest_memory_init(struct guest_memory *memory, int vm_fd, size_t size);
int guest_memory_load(struct guest_memory *memory, size_t address,
                      const void *source, size_t size);
void guest_memory_destroy(struct guest_memory *memory);

#endif

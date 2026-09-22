#ifndef GUEST_MEMORY_H
#define GUEST_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define GUEST_MEMORY_SIZE (64u * 1024u)

struct guest_memory {
    uint8_t *data;
    size_t size;
};

int guest_memory_init(struct guest_memory *memory, int vm_fd);
int guest_memory_load(struct guest_memory *memory, size_t address,
                      const void *source, size_t size);
void guest_memory_destroy(struct guest_memory *memory);

#endif

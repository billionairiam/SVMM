#ifndef BOOT_LINUX_H
#define BOOT_LINUX_H

#include <stddef.h>
#include <stdint.h>

struct guest_memory;

struct linux_image {
    uint8_t *data;
    size_t size;
    size_t setup_size;
    size_t kernel_size;
    uint32_t init_size;
};

int boot_linux_load(const char *path, struct linux_image *image);
size_t boot_linux_memory_size(const struct linux_image *image);
int boot_linux_prepare(struct guest_memory *memory, const struct linux_image *image,
                       const char *cmdline);
void boot_linux_free(struct linux_image *image);

#endif

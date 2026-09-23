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
    uint64_t runtime_start;
    uint32_t initrd_addr_max;
};

struct linux_initrd {
    uint64_t addr;
    size_t size;
};

int boot_linux_load(const char *path, struct linux_image *image);
size_t boot_linux_memory_size(const struct linux_image *image);
uint64_t boot_linux_initrd_addr(const struct linux_image *image);
int boot_linux_load_initramfs(struct guest_memory *memory, uint64_t address,
                              const char *path, size_t *size);
int boot_linux_prepare(struct guest_memory *memory, const struct linux_image *image,
                       const char *cmdline, const struct linux_initrd *initrd);
void boot_linux_free(struct linux_image *image);

#endif

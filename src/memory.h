#ifndef GUEST_MEMORY_H
#define GUEST_MEMORY_H

#include "ablation.h"

#include <stddef.h>
#include <stdint.h>

#define BLOG_GUEST_MEMORY_SIZE (32u * 1024u * 1024u)
#if SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED
#define GUEST_MEMORY_MIN_SIZE BLOG_GUEST_MEMORY_SIZE
#else
#define GUEST_MEMORY_MIN_SIZE (256u * 1024u * 1024u)
#endif
#define KERNEL_ADDR 0x00100000u
#define SETUP_CODE_ADDR 0x00010000u
#define BOOT_PARAMS_ADDR 0x00009000u
#define CMDLINE_ADDR 0x00020000u
#define CMDLINE_MAX_LEN 4096u
/*
 * initramfs 默认放在 96 MiB，避开 1 MiB 处的内核载荷和其后的解压窗口；
 * 若内核的 runtime_start + init_size 超过 96 MiB，加载器会顺延到窗口之后。
 */
#define INITRD_ADDR 0x06000000u
#define INITRD_MAX_SIZE 0x09000000u

struct guest_memory {
    uint8_t *data;
    size_t size;
};

int guest_memory_init(struct guest_memory *memory, int vm_fd, size_t size);
int guest_memory_load(struct guest_memory *memory, size_t address,
                      const void *source, size_t size);
void guest_memory_destroy(struct guest_memory *memory);

#endif

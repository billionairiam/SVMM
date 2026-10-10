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
/*
 * 32 位启动协议用的临时 GDT。boot_linux_prepare() 把描述符写到这里，
 * vcpu_setup_linux_boot() 让 GDTR 指向这里，并把 CS/DS 设成下面两个
 * selector。selector 的高 13 位是 GDT 下标，所以 0x10 是第 2 项、0x18 是第 3 项。
 */
#define BOOT_GDT_ADDR 0x00000500u
#define BOOT_GDT_ENTRIES 4u
#define BOOT_CS_SELECTOR 0x10u /* Linux 的 __BOOT_CS */
#define BOOT_DS_SELECTOR 0x18u /* Linux 的 __BOOT_DS */
/* 进入内核时的临时栈顶，位于 e820 可用区（64 KiB–1 MiB）内，向下增长。 */
#define BOOT_STACK_ADDR 0x00090000u
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

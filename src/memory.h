#ifndef GUEST_MEMORY_H
#define GUEST_MEMORY_H

#include "ablation.h"

#include <stddef.h>
#include <stdint.h>

#define MIB (1024u * 1024u)
/*
 * 客户机内存大小由运行时配置（mini-sandbox run --mem）决定，单位 MiB。
 * 下限 64 MiB：再小就放不下常见内核的解压窗口和用户态；上限 3 GiB：
 * 客户机 RAM 只映射在一个从 0 开始的内存槽里，必须停在 4 GiB 以下的
 * MMIO 区（IOAPIC 0xfec00000、LAPIC 0xfee00000）之前，ramdisk_image
 * 等启动字段也只有 32 位。
 */
#define DEFAULT_GUEST_MEM_MB 256u
#define GUEST_MEMORY_MAX_MB 3072u
/* fixed_32m 消融项忽略 --mem，固定使用博客 Stage 02 的 32 MiB。 */
#define BLOG_GUEST_MEMORY_MB 32u
#if SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED
#define GUEST_MEMORY_MIN_MB BLOG_GUEST_MEMORY_MB
#else
#define GUEST_MEMORY_MIN_MB 64u
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
 * initramfs 的位置随内存大小计算（见 boot_linux_initrd_addr()）：
 * 起点不低于 16 MiB，终点不高于内存末尾减去 16 MiB，顶部留给内核运行时。
 */
#define INITRD_MIN_ADDR (16u * MIB)
#define INITRD_TOP_RESERVE (16u * MIB)

struct guest_memory {
    uint8_t *data;
    size_t size;
};

/* 分配 size_mb MiB 客户机内存并注册为 KVM 内存槽 0（GPA 从 0 开始）。 */
int guest_memory_init(struct guest_memory *memory, int vm_fd, size_t size_mb);
int guest_memory_load(struct guest_memory *memory, size_t address,
                      const void *source, size_t size);
void guest_memory_destroy(struct guest_memory *memory);

#endif

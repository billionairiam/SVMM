#ifndef GUEST_MEMORY_H
#define GUEST_MEMORY_H

#include "ablation.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 客户机物理地址（GPA，Guest Physical Address）是客户机眼中的“物理内存
 * 地址”。本 VMM 只有一块从 GPA 0 开始的连续 RAM，宿主机上对应 mmap 得到
 * 的缓冲区，所以 GPA x 就是 memory->data + x。
 *
 * 教程使用固定的 32 MiB；现代内核解压后的运行窗口经常超过它，所以默认
 * 至少 128 MiB，实际大小由 boot_linux_memory_size() 根据内核计算。
 * fixed_32m 消融项恢复教程的 32 MiB 以便对比。
 */
#define BLOG_GUEST_MEMORY_SIZE (32u * 1024u * 1024u)
#if SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED
#define GUEST_MEMORY_MIN_SIZE BLOG_GUEST_MEMORY_SIZE
#else
#define GUEST_MEMORY_MIN_SIZE (128u * 1024u * 1024u)
#endif

/*
 * 启动时的客户机物理内存布局，完整示意图见 boot_linux_prepare()。
 * 这些地址沿用 PC/Linux 的传统位置，彼此不重叠：
 */
/* 0x0–0x4ff 在真实 PC 上是实模式中断向量表和 BIOS 数据区，GDT 放在其后。 */
#define GDT_ADDR 0x00000500u
/*
 * 1 MiB：bzImage 保护模式部分（解压器 + 压缩内核）的标准载入地址，
 * 也是 32 位入口地址。1 MiB 以下在真实 PC 上被显存和 BIOS ROM 占用，
 * 所以 Linux 约定内核放在 1 MiB 以上（启动头 LOADED_HIGH 标志）。
 */
#define KERNEL_ADDR 0x00100000u
/* 64 KiB：bzImage 前半段（引导扇区 + setup）的副本，最长 64 KiB。 */
#define SETUP_CODE_ADDR 0x00010000u
/* 36 KiB：struct boot_params（零页，4 KiB），vCPU 启动时 RSI 指向这里。 */
#define BOOT_PARAMS_ADDR 0x00009000u
/* 128 KiB：以 NUL 结尾的内核命令行，boot_params.hdr.cmd_line_ptr 指向这里。 */
#define CMDLINE_ADDR 0x00020000u
/* 本 VMM 允许的命令行最大长度（不含 NUL），还会受内核 cmdline_size 限制。 */
#define CMDLINE_MAX_LEN 4096u

/* 客户机 RAM：宿主机地址 data 对应 GPA 0，共 size 字节。 */
struct guest_memory {
    uint8_t *data;
    size_t size;
};

int guest_memory_init(struct guest_memory *memory, int vm_fd, size_t size);
int guest_memory_load(struct guest_memory *memory, size_t address,
                      const void *source, size_t size);
void guest_memory_destroy(struct guest_memory *memory);

#endif

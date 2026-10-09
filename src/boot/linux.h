#ifndef BOOT_LINUX_H
#define BOOT_LINUX_H

/*
 * Linux bzImage 加载器：让 VMM 扮演“最小引导加载器”。
 *
 * 一、真实 PC 上 Linux 是怎么被启动的
 *
 *   上电后 CPU 不会直接运行 Linux，而是先运行主板上的固件：
 *
 *   - BIOS：传统 PC 固件。它初始化内存控制器、发现磁盘等设备，并在
 *     中断向量表里提供一组 16 位“中断服务”，例如 int 0x10 显示字符、
 *     int 0x13 读磁盘、int 0x15 (AX=0xe820) 查询物理内存布局。启动时它把
 *     磁盘第一个扇区（MBR，512 字节）读到 0x7c00 并跳过去。
 *   - UEFI：BIOS 的现代替代品。它不读 MBR，而是从 EFI 系统分区（FAT 格式）
 *     里找一个 PE/COFF 格式的可执行文件（“EFI 应用”）来运行，并通过
 *     函数指针表（Boot Services / Runtime Services）而不是 int 中断提供服务。
 *
 *   固件之后通常是引导加载器 GRUB：它会读文件系统、找到内核文件
 *   vmlinuz（就是 bzImage）和 initrd，按“Linux x86 启动协议”把它们放进
 *   内存、填好参数，然后跳进内核。完整链条：
 *
 *     真实机器：  BIOS/UEFI -> GRUB -> Linux setup 代码 -> 解压器 -> 内核
 *     QEMU -kernel： QEMU 自己解析 bzImage -> setup 代码 -> 解压器 -> 内核
 *     SVMM：       本 VMM 解析 bzImage -------------------> 解压器 -> 内核
 *
 * 二、本 VMM 为什么要自己做这些事
 *
 *   KVM 只虚拟 CPU 和内存，不会往虚拟机里放任何固件。所以这里既没有
 *   BIOS 的 int 中断服务，也没有 UEFI 的服务表。GRUB 原本做的事情——
 *   把内核放到约定地址、告诉内核内存布局和命令行——只能由 VMM 直接
 *   写进客户机内存。这正是 QEMU 的 `-kernel` 选项所做的事。
 *
 *   SVMM 还跳过了 bzImage 里的 16 位 setup 代码：setup 代码会调用 BIOS
 *   中断收集硬件信息，没有 BIOS 就会跑飞。Linux 为此定义了“32 位启动
 *   协议”：引导器自己准备好 setup 本来要收集的信息（boot_params），
 *   直接以 32 位保护模式跳进压缩内核。详见 vcpu_setup_linux_boot()。
 *
 * 三、bzImage 是什么
 *
 *   "bzImage" 是 "big zImage"，与 bzip2 无关。它不是 ELF，也不是一个
 *   普通压缩包，而是 Linux x86 启动协议规定的镜像格式，按 512 字节扇区
 *   分成两段（详细字节布局见 linux.c 中 boot_linux_load() 的注释）：
 *
 *     [ boot sector + setup_header + 16 位 setup 代码 ][ 保护模式部分 ]
 *       共 (setup_sects + 1) * 512 字节                   文件的剩余部分
 *
 *   “保护模式部分”开头是一段 32/64 位的解压器代码（入口 startup_32），
 *   后面跟着压缩过的真正内核（vmlinux）。所以 VMM 不需要会解压：把它
 *   放到 1 MiB 并跳到开头，内核会自己解压、搬移并启动。
 *
 *   如果内核开启了 CONFIG_EFI_STUB（发行版内核基本都开），同一个文件
 *   的开头还是一个合法的 PE/COFF 头（以 "MZ" 开头），UEFI 固件可以把
 *   bzImage 当作 EFI 应用直接运行，不需要 GRUB。这个 "EFI stub" 会用
 *   UEFI 服务获取内存表等信息，再把它们转换成 boot_params 交给内核。
 *   SVMM 没有 UEFI，不使用这条路径：PE 头对我们只是 setup 段里的普通
 *   字节，原样复制即可。
 */

#include <stddef.h>
#include <stdint.h>

struct guest_memory;

/* boot_linux_load() 解析后的 bzImage，全部位于宿主机内存。 */
struct linux_image {
    /* 整个 bzImage 文件的内容，由 boot_linux_free() 释放。 */
    uint8_t *data;
    /* 文件总字节数。 */
    size_t size;
    /* 前半段（boot sector + setup）的字节数，也是保护模式部分的文件偏移。 */
    size_t setup_size;
    /* 后半段（解压器 + 压缩内核）的字节数，会被复制到 KERNEL_ADDR。 */
    size_t kernel_size;
    /* 启动头 init_size：内核解压和早期初始化需要的连续内存字节数。 */
    uint32_t init_size;
    /* 内核解压后实际运行的物理地址，init_size 从这里开始算。 */
    uint64_t runtime_start;
};

/* 读取并校验 bzImage。成功返回 0；失败返回 -1，已打印原因。 */
int boot_linux_load(const char *path, struct linux_image *image);
/* 根据内核的运行地址和 init_size 计算应分配多少客户机内存。 */
size_t boot_linux_memory_size(const struct linux_image *image);
/*
 * 扮演引导加载器：把内核、setup 副本、命令行、boot_params 和 GDT 写进
 * 客户机内存。成功返回 0；失败返回 -1 并设置 errno。
 */
int boot_linux_prepare(struct guest_memory *memory, const struct linux_image *image,
                       const char *cmdline);
/* 释放 boot_linux_load() 分配的缓冲区，可重复调用。 */
void boot_linux_free(struct linux_image *image);

#endif

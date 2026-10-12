#ifndef SANDBOX_H
#define SANDBOX_H

#include <stddef.h>

#define SANDBOX_DEFAULT_CMDLINE \
    "console=ttyS0 earlycon=uart8250,io,0x3f8 init=/init loglevel=8 " \
    "acpi_rsdp=0xe0000 reboot=acpi panic=-1"

/*
 * SandboxSpec：声明“要一个什么样的 sandbox”，不包含任何运行时资源。
 * 字符串只被借用，sandbox_run() 返回前调用者要保证它们有效。
 *
 * 与 Firecracker REST API 的对应关系：
 *   kernel_path  PUT /boot-source   kernel_image_path
 *   initrd_path  PUT /boot-source   initrd_path
 *   mem_size_mb  PUT /machine-config mem_size_mib
 *   cmdline      PUT /boot-source   boot_args
 */
struct sandbox_config {
    /* x86 bzImage 路径，必填。 */
    const char *kernel_path;
    /* initramfs（cpio 或 cpio.gz）路径；NULL 或空串表示不加载。 */
    const char *initrd_path;
    /* 客户机内存大小，单位 MiB，范围见 memory.h。 */
    size_t mem_size_mb;
    /* 内核命令行。 */
    const char *cmdline;
};

/* 填入默认值：256 MiB、默认命令行、无内核和 initramfs。 */
void sandbox_config_init(struct sandbox_config *config);

/*
 * 把 spec 实例化成一台 VM 并运行到结束（相当于 Firecracker 的
 * InstanceStart + 等待退出）。KVM fd、客户机内存、vCPU、串口和控制台
 * 这些 SandboxInstance 资源都在函数内部创建和释放，不暴露给调用者。
 *
 * 客户机正常结束（poweroff、reset、Ctrl-A x、SIGINT/SIGTERM/SIGHUP）返回 0；
 * 配置无法满足、KVM 出错或客户机三重故障返回 1。
 */
int sandbox_run(const struct sandbox_config *config);

#endif

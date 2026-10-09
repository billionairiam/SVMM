/*
 * Stage 02 入口：用最小 VMM 直接启动 Linux bzImage。
 *
 * 本程序在宿主机上扮演三种角色（真实 PC 上它们是不同的软件）：
 *   - 机器：用 KVM 创建虚拟机、客户机 RAM 和 vCPU（kvm.c、memory.c、vcpu.c）；
 *   - 引导加载器：代替 BIOS/UEFI + GRUB 把内核和启动参数放进内存
 *     （boot/linux.c）；
 *   - 设备：在 VM exit 中模拟串口和 RTC（serial.c、rtc.c）。
 *
 * 执行顺序：
 *   1. boot_linux_load()        读取并校验 bzImage 启动头
 *   2. kvm_context_init()       打开 /dev/kvm，创建虚拟机
 *   3. guest_memory_init()      按内核需要分配客户机 RAM
 *   4. boot_linux_prepare()     写入内核、命令行、boot_params、e820、GDT
 *   5. vcpu_init()              创建 vCPU，设置 CPUID
 *   6. vcpu_setup_linux_boot()  设置 32 位保护模式寄存器，RIP = 1 MiB
 *   7. vcpu_run()               反复 KVM_RUN，处理 I/O，直到客户机 HLT
 *
 * 用法：linux_boot [bzImage]。镜像路径也可以用环境变量 BZIMAGE_PATH 指定，
 * 默认 images/bzImage；内核命令行可以用环境变量 CMDLINE 覆盖。
 */
#include "ablation.h"
#include "boot/linux.h"
#include "kvm.h"
#include "memory.h"
#include "metrics.h"
#include "rtc.h"
#include "serial.h"
#include "vcpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void metric_failure(const char *reason, size_t guest_memory_bytes)
{
    char stage[64];
    snprintf(stage, sizeof(stage), "failed_%s", reason);
    metric_stage(stderr, SVMM_VARIANT_NAME, stage, guest_memory_bytes);
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: %s [bzImage]\n", argv[0]);
        return 2;
    }
    const char *path = argc == 2 ? argv[1] : getenv("BZIMAGE_PATH");
    if (!path || !*path)
        path = "images/bzImage";
    /*
     * console=ttyS0 让内核把控制台日志输出到第一个串口（COM1，0x3f8）；
     * earlycon 让 8250 串口驱动加载之前的早期日志也走同一个端口。
     */
    const char *cmdline = getenv("CMDLINE");
    if (!cmdline)
        cmdline = "console=ttyS0 earlycon=uart8250,io,0x3f8";

    struct kvm_context kvm = { .system_fd = -1, .vm_fd = -1 };
    struct guest_memory memory = { 0 };
    struct vcpu vcpu = { .fd = -1 };
    struct serial serial;
    struct rtc rtc;
    struct linux_image image = { 0 };
    size_t memory_size = 0;
    int status = 1;

    if (boot_linux_load(path, &image) < 0) {
        metric_failure("image_load", memory_size);
        goto done;
    }
    metric_stage(stderr, SVMM_VARIANT_NAME, "image_loaded", memory_size);

    if (kvm_context_init(&kvm) < 0) {
        metric_failure("kvm_init", memory_size);
        goto done;
    }

    memory_size = boot_linux_memory_size(&image);
    metric_stage(stderr, SVMM_VARIANT_NAME, "memory", memory_size);
    if (guest_memory_init(&memory, kvm.vm_fd, memory_size) < 0) {
        metric_failure("guest_memory", memory_size);
        goto done;
    }
    metric_stage(stderr, SVMM_VARIANT_NAME, "memory_mapped", memory_size);

    if (boot_linux_prepare(&memory, &image, cmdline) < 0) {
        perror("prepare Linux boot");
        metric_failure("boot_prepare", memory_size);
        goto done;
    }
    metric_stage(stderr, SVMM_VARIANT_NAME, "boot_prepared", memory_size);

    if (vcpu_init(&vcpu, &kvm, 0) < 0) {
        metric_failure("vcpu_init", memory_size);
        goto done;
    }
    if (vcpu_setup_linux_boot(&vcpu, KERNEL_ADDR, BOOT_PARAMS_ADDR) < 0) {
        metric_failure("vcpu_setup", memory_size);
        goto done;
    }
    metric_stage(stderr, SVMM_VARIANT_NAME, "vcpu_setup", memory_size);

    serial_init(&serial, STDOUT_FILENO);
    rtc_init(&rtc);
    struct vcpu_devices devices = { .serial = &serial, .rtc = &rtc };
    /*
     * 本阶段没有根文件系统，内核最终会停机（HLT）或在 VFS panic 中空转；
     * 到达 HLT 就算本阶段完成。
     */
    struct vcpu_run_stats run_stats = { 0 };
    metric_stage(stderr, SVMM_VARIANT_NAME, "kvm_run", memory_size);
    int run_result = vcpu_run(&vcpu, &devices, &run_stats);
    if (run_result == 0) {
        fprintf(stderr, "INFO: Stage 02 completed\n");
        status = 0;
    }

done:
    vcpu_destroy(&vcpu);
    guest_memory_destroy(&memory);
    kvm_context_destroy(&kvm);
    boot_linux_free(&image);
    return status;
}

#include "ablation.h"
#include "boot/acpi.h"
#include "boot/linux.h"
#include "console.h"
#include "kvm.h"
#include "memory.h"
#include "metrics.h"
#include "serial.h"
#include "vcpu.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * reboot=acpi：HW-reduced ACPI 且没有 EFI 时，x86 默认把重启方式改成 EFI，
 * EFI 不可用又退回 BIOS 实模式跳转，而这里没有 BIOS。显式指定 ACPI 后，
 * panic 等紧急重启路径直接写 FADT 的 RESET_REG（0xcf9）。
 */
#define DEFAULT_CMDLINE \
    "console=ttyS0 earlycon=uart8250,io,0x3f8 init=/init loglevel=8 " \
    "acpi_rsdp=0xe0000 reboot=acpi panic=-1"

static struct vcpu *signal_vcpu;

static void metric_failure(const char *reason, size_t guest_memory_bytes)
{
    char stage[64];
    snprintf(stage, sizeof(stage), "failed_%s", reason);
    metric_stage(stderr, SVMM_VARIANT_NAME, stage, guest_memory_bytes);
}

static void wake_handler(int signo)
{
    (void)signo;
}

static void stop_handler(int signo)
{
    (void)signo;
    if (signal_vcpu)
        vcpu_request_stop(signal_vcpu);
}

/*
 * SIGUSR1 只用来把 vCPU 线程从 KVM_RUN 中打断；SIGINT/SIGTERM/SIGHUP 让
 * VMM 走正常清理路径退出，保证终端从 raw 模式恢复。不设置 SA_RESTART，
 * 这样被打断的 KVM_RUN 会返回 EINTR。
 */
static void install_signal_handlers(struct vcpu *vcpu)
{
    signal_vcpu = vcpu;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = wake_handler;
    sigaction(SIGUSR1, &action, NULL);
    action.sa_handler = stop_handler;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
}

static void serial_irq(void *opaque, int level)
{
    kvm_set_irq_line(opaque, COM1_IRQ, level);
}

static void console_quit(void *opaque)
{
    vcpu_request_stop(opaque);
}

int main(int argc, char **argv)
{
    if (argc > 3) {
        fprintf(stderr, "usage: %s [bzImage [initramfs]]\n", argv[0]);
        return 2;
    }
    const char *path = argc >= 2 ? argv[1] : getenv("BZIMAGE_PATH");
    if (!path || !*path)
        path = "images/bzImage";
    /* INITRAMFS_PATH 为空字符串时不加载 initramfs，回到 Stage 02 的行为。 */
    const char *initramfs_path = argc == 3 ? argv[2] : getenv("INITRAMFS_PATH");
    if (!initramfs_path)
        initramfs_path = "build/initramfs.cpio.gz";
    const char *cmdline = getenv("CMDLINE");
    if (!cmdline)
        cmdline = DEFAULT_CMDLINE;

    struct kvm_context kvm = { .system_fd = -1, .vm_fd = -1 };
    struct guest_memory memory = { 0 };
    struct vcpu vcpu = { .fd = -1 };
    struct serial serial;
    struct console console = { 0 };
    struct linux_image image = { 0 };
    size_t memory_size = 0;
    int serial_ready = 0;
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

    if (boot_acpi_setup(&memory) < 0) {
        perror("ACPI tables");
        metric_failure("acpi_setup", memory_size);
        goto done;
    }

    struct linux_initrd initrd = { 0 };
    if (*initramfs_path) {
        initrd.addr = boot_linux_initrd_addr(&image);
        if (boot_linux_load_initramfs(&memory, initrd.addr, initramfs_path,
                                      &initrd.size) < 0) {
            metric_failure("initramfs_load", memory_size);
            goto done;
        }
        fprintf(stderr, "INFO: initramfs %s loaded at 0x%llx (%zu bytes)\n",
                initramfs_path, (unsigned long long)initrd.addr, initrd.size);
    }
    if (boot_linux_prepare(&memory, &image, cmdline, &initrd) < 0) {
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
    serial_ready = 1;
    serial_set_irq_handler(&serial, serial_irq, &kvm);
    install_signal_handlers(&vcpu);
    if (console_start(&console, &serial, STDIN_FILENO, console_quit, &vcpu) < 0) {
        metric_failure("console", memory_size);
        goto done;
    }

    struct vcpu_run_stats run_stats = { 0 };
    metric_stage(stderr, SVMM_VARIANT_NAME, "kvm_run", memory_size);
    int run_result = vcpu_run(&vcpu, &serial, &run_stats);
    console_stop(&console);
    if (run_result == 0) {
        fprintf(stderr, "INFO: Stage 03 completed\n");
        status = 0;
    }

done:
    console_stop(&console);
    signal_vcpu = NULL;
    vcpu_destroy(&vcpu);
    if (serial_ready)
        serial_destroy(&serial);
    guest_memory_destroy(&memory);
    kvm_context_destroy(&kvm);
    boot_linux_free(&image);
    return status;
}

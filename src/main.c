#include "ablation.h"
#include "boot/linux.h"
#include "kvm.h"
#include "memory.h"
#include "metrics.h"
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
    const char *cmdline = getenv("CMDLINE");
    if (!cmdline)
        cmdline = "console=ttyS0 earlycon=uart8250,io,0x3f8";

    struct kvm_context kvm = { .system_fd = -1, .vm_fd = -1 };
    struct guest_memory memory = { 0 };
    struct vcpu vcpu = { .fd = -1 };
    struct serial serial;
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
    struct vcpu_run_stats run_stats = { 0 };
    metric_stage(stderr, SVMM_VARIANT_NAME, "kvm_run", memory_size);
    int run_result = vcpu_run(&vcpu, &serial, &run_stats);
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

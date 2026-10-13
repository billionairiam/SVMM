#include "sandbox.h"

#include "ablation.h"
#include "boot/acpi.h"
#include "boot/linux.h"
#include "console.h"
#include "kvm.h"
#include "memory.h"
#include "metrics.h"
#include "serial.h"
#include "vcpu.h"
#include "virtio/blk.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * sandbox_run() 把 sandbox_config 实例化为一台 VM 并运行到结束。
 * 逻辑上的 SandboxInstance 就是下面这些局部变量：KVM fd、客户机内存、
 * vCPU、串口和控制台。它们全部先初始化成“空”状态，任何一步失败都
 * goto done，由同一段清理代码释放；每个 destroy 函数对未初始化或
 * 部分初始化的对象都是安全的。
 */

static struct vcpu *signal_vcpu;

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

static void blk_irq(void *opaque, int level)
{
    kvm_set_irq_line(opaque, VIRTIO_MMIO_IRQ, level);
}

static void blk_mmio_access(void *opaque, uint64_t offset, uint8_t *data, uint32_t len,
                            int is_write)
{
    virtio_mmio_access(opaque, offset, data, len, is_write);
}

static void console_quit(void *opaque)
{
    vcpu_request_stop(opaque);
}

static void metric_failure(const char *reason, size_t guest_memory_bytes)
{
    char stage[64];
    snprintf(stage, sizeof(stage), "failed_%s", reason);
    metric_stage(stderr, SVMM_VARIANT_NAME, stage, guest_memory_bytes);
}

/* boot_linux_prepare() 失败时，给出比 EINVAL 更有用的原因。 */
static void report_prepare_failure(const struct linux_image *image, size_t memory_size)
{
    uint64_t kernel_end = image->runtime_start + image->init_size;
    if (kernel_end > memory_size)
        fprintf(stderr,
                "ERROR: the kernel decompresses into [0x%llx, 0x%llx), beyond %zu MiB "
                "of guest memory; try a larger --mem\n",
                (unsigned long long)image->runtime_start,
                (unsigned long long)kernel_end, memory_size / MIB);
    else
        perror("prepare Linux boot");
}

void sandbox_config_init(struct sandbox_config *config)
{
    *config = (struct sandbox_config){
        .mem_size_mb = DEFAULT_GUEST_MEM_MB,
        .cmdline = SANDBOX_DEFAULT_CMDLINE,
    };
}

int sandbox_run(const struct sandbox_config *config)
{
    struct kvm_context kvm = { .system_fd = -1, .vm_fd = -1 };
    struct guest_memory memory = { 0 };
    struct vcpu vcpu = { .fd = -1 };
    struct serial serial;
    struct console console = { 0 };
    struct linux_image image = { 0 };
    struct vcpu_run_stats run_stats = { 0 };
    struct virtio_blk blk = { .fd = -1 };
    int blk_attached = 0;
    size_t mem_size_mb = SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED ?
        BLOG_GUEST_MEMORY_MB : config->mem_size_mb;
    size_t memory_size = 0;
    int serial_ready = 0;
    int status = 1;

    /* 1. 读取并校验 bzImage。放在打开 /dev/kvm 之前，镜像有问题时尽早失败。 */
    if (boot_linux_load(config->kernel_path, &image) < 0) {
        metric_failure("image_load", memory_size);
        goto done;
    }
    metric_stage(stderr, SVMM_VARIANT_NAME, "image_loaded", memory_size);

    /* 1b. 可选的磁盘镜像：同样在创建 VM 之前打开，路径错误或被占用时尽早失败。 */
    if (config->disk_path) {
        if (virtio_blk_open(&blk, config->disk_path) < 0) {
            metric_failure("disk_open", memory_size);
            goto done;
        }
    }

    /* 2. 打开 /dev/kvm，创建 VM、内核 irqchip 和 PIT。 */
    if (kvm_context_init(&kvm) < 0) {
        metric_failure("kvm_init", memory_size);
        goto done;
    }

    /* 3. 按 --mem 分配客户机内存并注册给 KVM。 */
    memory_size = mem_size_mb * MIB;
    metric_stage(stderr, SVMM_VARIANT_NAME, "memory", memory_size);
    if (guest_memory_init(&memory, kvm.vm_fd, mem_size_mb) < 0) {
        metric_failure("guest_memory", memory_size);
        goto done;
    }
    metric_stage(stderr, SVMM_VARIANT_NAME, "memory_mapped", memory_size);

    /*
     * 3b. virtio-blk 接到客户机内存和 GSI 5 上。寄存器页在 0xd0000000，
     * 不注册为内存槽，客户机访问它时产生 KVM_EXIT_MMIO。
     */
    if (config->disk_path) {
        virtio_blk_attach(&blk, memory.data, memory.size, blk_irq, &kvm);
        blk_attached = 1;
        fprintf(stderr, "INFO: virtio-blk %s: %llu sectors, mmio 0x%x irq %u\n",
                config->disk_path, (unsigned long long)blk.config.capacity,
                VIRTIO_MMIO_BASE, VIRTIO_MMIO_IRQ);
    }

    /* 4. ACPI 表（RSDP 在 0xE0000）；带磁盘时 DSDT 里多一个 virtio-mmio 设备。 */
    const struct acpi_virtio_mmio acpi_blk = {
        .base = VIRTIO_MMIO_BASE, .size = VIRTIO_MMIO_SIZE, .gsi = VIRTIO_MMIO_IRQ,
    };
    if (boot_acpi_setup(&memory, blk_attached ? &acpi_blk : NULL) < 0) {
        perror("ACPI tables");
        metric_failure("acpi_setup", memory_size);
        goto done;
    }

    /* 5. 可选的 initramfs：地址和上限都随内存大小计算。 */
    struct linux_initrd initrd = { 0 };
    if (config->initrd_path && *config->initrd_path) {
        initrd.addr = boot_linux_initrd_addr(&image, memory.size);
        uint64_t limit = boot_linux_initrd_limit(&image, memory.size);
        if (boot_linux_load_initramfs(&memory, config->initrd_path, initrd.addr,
                                      limit, &initrd.size) < 0) {
            metric_failure("initramfs_load", memory_size);
            goto done;
        }
        fprintf(stderr, "INFO: initramfs %s loaded at 0x%llx (%zu bytes)\n",
                config->initrd_path, (unsigned long long)initrd.addr, initrd.size);
    }

    /* 6. 内核载荷、zero page（e820 覆盖整个 --mem）、命令行和 GDT。 */
    if (boot_linux_prepare(&memory, &image, config->cmdline, &initrd) < 0) {
        report_prepare_failure(&image, memory.size);
        metric_failure("boot_prepare", memory_size);
        goto done;
    }
    metric_stage(stderr, SVMM_VARIANT_NAME, "boot_prepared", memory_size);

    /* 7. vCPU：CPUID、32 位保护模式寄存器，RIP 指向 1 MiB 处的内核入口。 */
    if (vcpu_init(&vcpu, &kvm, 0) < 0) {
        metric_failure("vcpu_init", memory_size);
        goto done;
    }
    if (vcpu_setup_linux_boot(&vcpu, KERNEL_ADDR, BOOT_PARAMS_ADDR) < 0) {
        metric_failure("vcpu_setup", memory_size);
        goto done;
    }
    if (blk_attached)
        vcpu.mmio = (struct vcpu_mmio_region){
            .base = VIRTIO_MMIO_BASE,
            .size = VIRTIO_MMIO_SIZE,
            .access = blk_mmio_access,
            .opaque = &blk.mmio,
        };
    metric_stage(stderr, SVMM_VARIANT_NAME, "vcpu_setup", memory_size);

    /* 8. 串口输出到 stdout，控制台把 stdin 送进串口。 */
    serial_init(&serial, STDOUT_FILENO);
    serial_ready = 1;
    serial_set_irq_handler(&serial, serial_irq, &kvm);
    install_signal_handlers(&vcpu);
    if (console_start(&console, &serial, STDIN_FILENO, console_quit, &vcpu) < 0) {
        metric_failure("console", memory_size);
        goto done;
    }

    /* 9. KVM_RUN 循环，直到客户机关机/复位或宿主机请求停止。 */
    metric_stage(stderr, SVMM_VARIANT_NAME, "kvm_run", memory_size);
    if (vcpu_run(&vcpu, &serial, &run_stats) == 0)
        status = 0;

done:
    /* 10. 清理。先恢复终端模式，再打印统计；raw 模式下 "\n" 不会回到行首。 */
    console_stop(&console);
    metric_exit_breakdown(stderr, &run_stats);
    if (blk_attached)
        virtio_blk_print_stats(&blk, stderr);
    if (status == 0)
        fprintf(stderr, "INFO: sandbox completed\n");
    signal_vcpu = NULL;
    vcpu_destroy(&vcpu);
    if (serial_ready)
        serial_destroy(&serial);
    guest_memory_destroy(&memory);
    virtio_blk_close(&blk);
    kvm_context_destroy(&kvm);
    boot_linux_free(&image);
    return status;
}

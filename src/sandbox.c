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
#include "virtio/net.h"

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
 *
 * 运行起来后整台机器是这样的：
 *
 *   宿主进程 svmm                               客户机（Linux）
 *   ┌───────────────────────────────────┐      ┌──────────────────────────┐
 *   │ 主线程 = vCPU 线程                  │      │                          │
 *   │   vcpu_run(): ioctl(KVM_RUN) 循环 ◄─┼──────┤ IN/OUT 0x3f8  → 串口模型   │
 *   │     ├ KVM_EXIT_IO   → serial.c     │ exit │ MMIO 0xd0000000 → virtio  │
 *   │     └ KVM_EXIT_MMIO → virtio/mmio.c│      │ 0xd0001000 → virtio-net   │
 *   │                                   │      │ 普通内存访问 → 不退出       │
 *   │                                   │      │                          │
 *   │ 控制台线程 console.c                │      │                          │
 *   │   读 stdin → 串口接收 FIFO ─────────┼─IRQ─►│ 8250 驱动                 │
 *   │                                   │      │                          │
 *   │ guest_memory (mmap) ◄─────────────┼──────┤ 客户机 RAM，GPA 0..mem     │
 *   │ 磁盘镜像 fd (pread/pwrite)          │      │ /dev/vda                 │
 *   │                                   │      │                          │
 *   │ virtio-net RX 线程 net.c            │      │                          │
 *   │   tap fd 可读 → 写 RX buffer ───────┼─IRQ─►│ eth0 (virtio_net)        │
 *   │ TX 在 vCPU 线程上 writev(tap) ◄─────┼─exit─┤                          │
 *   └───────────────────────────────────┘      └──────────────────────────┘
 *
 * KVM 内核模块负责 CPU 虚拟化、内存（EPT）、LAPIC/IOAPIC/PIT 和 HLT；VMM 只
 * 模拟 KVM 不认识的设备。设备通知客户机的唯一方式是 kvm_set_irq_line()。
 *
 * 各模块之间用“函数指针 + void *opaque”的回调互相连接，彼此不需要包含对方的
 * 头文件：例如 serial.c 不知道 KVM 的存在，它只在中断电平变化时调用
 * serial_irq(opaque=&kvm, level)，由这里转成 KVM_IRQ_LINE。下面几个 static
 * 函数就是这些“接线”。
 */

/* 信号处理函数只能访问全局变量，所以把 vCPU 指针放在这里。 */
static struct vcpu *signal_vcpu;

/* 什么都不做：SIGUSR1 的作用只是让阻塞中的 KVM_RUN 以 EINTR 返回。 */
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

/* 串口模型的中断输出 → KVM 的 GSI 4（COM1 的传统 IRQ）。 */
static void serial_irq(void *opaque, int level)
{
    kvm_set_irq_line(opaque, COM1_IRQ, level);
}

/* virtio-mmio 传输层的中断输出 → GSI 5，与 DSDT 里 \_SB.VR00 声明的一致。 */
static void blk_irq(void *opaque, int level)
{
    kvm_set_irq_line(opaque, VIRTIO_MMIO_IRQ, level);
}

/*
 * virtio-net 的中断输出 → GSI 6（\_SB.VR01）。由 vCPU 线程（TX 完成）或
 * RX 线程（收到帧）调用；KVM_IRQ_LINE 可以在任何线程上调用。
 */
static void net_irq(void *opaque, int level)
{
    kvm_set_irq_line(opaque, VIRTIO_MMIO_NET_IRQ, level);
}

/*
 * vcpu.c 收到落在某个 virtio 寄存器页内的 KVM_EXIT_MMIO 时调用它。
 * opaque 是 &blk.mmio 或 &net.mmio；这层转发让 vcpu.c 不必知道 virtio 的存在。
 */
static void virtio_mmio_exit(void *opaque, uint64_t offset, uint8_t *data, uint32_t len,
                            int is_write)
{
    virtio_mmio_access(opaque, offset, data, len, is_write);
}

/* 控制台线程收到 Ctrl-A x 时调用：让 vCPU 线程退出 KVM_RUN 循环。 */
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
    /* rx_frame 有 64 KiB，放在栈上也没问题（默认栈 8 MiB）。 */
    struct virtio_net net = { .tap_fd = -1, .wake_fd = -1 };
    int net_attached = 0;
    struct acpi_virtio_mmio acpi_devices[ACPI_VIRTIO_MMIO_MAX];
    unsigned acpi_count = 0;
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

    /*
     * 1c. 可选的 TAP 设备。tap 必须已经存在（scripts/setup-tap.sh 创建），
     * 不存在、被别的进程占用或不是 TAP 时在这里失败，不会启动一个没有网络的 VM。
     */
    if (config->tap_name) {
        if (virtio_net_open(&net, config->tap_name) < 0) {
            metric_failure("net_open", memory_size);
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
     * 设备需要客户机内存的宿主指针（memory.data）来直接读写 virtqueue 和
     * 数据 buffer，所以必须在第 3 步之后。
     */
    if (config->disk_path) {
        virtio_blk_attach(&blk, memory.data, memory.size, blk_irq, &kvm);
        blk_attached = 1;
        fprintf(stderr, "INFO: virtio-blk %s: %llu sectors, mmio 0x%x irq %u\n",
                config->disk_path, (unsigned long long)blk.config.capacity,
                VIRTIO_MMIO_BASE, VIRTIO_MMIO_IRQ);
        acpi_devices[acpi_count++] = (struct acpi_virtio_mmio){
            .base = VIRTIO_MMIO_BASE, .size = VIRTIO_MMIO_SIZE, .gsi = VIRTIO_MMIO_IRQ,
        };
    }

    /*
     * 3c. virtio-net 接到 GSI 6，寄存器页紧跟在 blk 后面（0xd0001000）。
     * 没有磁盘时地址也不变：每个设备的地址/GSI 固定，便于排查。
     */
    if (config->tap_name) {
        virtio_net_attach(&net, memory.data, memory.size, net_irq, &kvm);
        net_attached = 1;
        const uint8_t *mac = net.config.mac;
        fprintf(stderr, "INFO: virtio-net %s: mac %02x:%02x:%02x:%02x:%02x:%02x, "
                "mmio 0x%x irq %u\n", net.ifname, mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5], VIRTIO_MMIO_NET_BASE, VIRTIO_MMIO_NET_IRQ);
        acpi_devices[acpi_count++] = (struct acpi_virtio_mmio){
            .base = VIRTIO_MMIO_NET_BASE, .size = VIRTIO_MMIO_SIZE, .gsi = VIRTIO_MMIO_NET_IRQ,
        };
    }

    /*
     * 4. ACPI 表（RSDP 在 0xE0000）；每个 virtio 设备在 DSDT 里多一个 virtio-mmio 设备。
     * virtio-mmio 不像 PCI 那样能被扫描发现，客户机内核只能从 ACPI（或设备树、
     * 命令行）得知“0xd0000000 有一个设备、用 GSI 5”。VMM 和客户机对设备位置的
     * 约定全靠这张表。
     */
    if (boot_acpi_setup(&memory, acpi_devices, acpi_count) < 0) {
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
    /*
     * 把 virtio 寄存器页登记给 vCPU 的 MMIO 分派。KVM 本身不知道哪个地址属于
     * 哪个设备，它只报告“客户机访问了某个未映射的 GPA”，由 VMM 自己查表。
     */
    if ((blk_attached && vcpu_mmio_register(&vcpu, VIRTIO_MMIO_BASE, VIRTIO_MMIO_SIZE,
                                            virtio_mmio_exit, &blk.mmio) < 0) ||
        (net_attached && vcpu_mmio_register(&vcpu, VIRTIO_MMIO_NET_BASE, VIRTIO_MMIO_SIZE,
                                            virtio_mmio_exit, &net.mmio) < 0)) {
        metric_failure("vcpu_setup", memory_size);
        goto done;
    }
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

    /*
     * 8b. virtio-net 的 RX 线程。它在 driver 写 DRIVER_OK 之前只等 eventfd，
     * 不会碰客户机内存。
     */
    if (net_attached && virtio_net_start(&net) < 0) {
        metric_failure("net_start", memory_size);
        goto done;
    }

    /* 9. KVM_RUN 循环，直到客户机关机/复位或宿主机请求停止。 */
    metric_stage(stderr, SVMM_VARIANT_NAME, "kvm_run", memory_size);
    if (vcpu_run(&vcpu, &serial, &run_stats) == 0)
        status = 0;

done:
    /* 10. 清理。先恢复终端模式，再打印统计；raw 模式下 "\n" 不会回到行首。 */
    console_stop(&console);
    /* RX 线程会写客户机内存，必须在 guest_memory_destroy() 之前停下。 */
    virtio_net_stop(&net);
    metric_exit_breakdown(stderr, &run_stats);
    if (blk_attached)
        virtio_blk_print_stats(&blk, stderr);
    if (net_attached)
        virtio_net_print_stats(&net, stderr);
    if (status == 0)
        fprintf(stderr, "INFO: sandbox completed\n");
    signal_vcpu = NULL;
    vcpu_destroy(&vcpu);
    if (serial_ready)
        serial_destroy(&serial);
    guest_memory_destroy(&memory);
    virtio_blk_close(&blk);
    virtio_net_close(&net);
    kvm_context_destroy(&kvm);
    boot_linux_free(&image);
    return status;
}

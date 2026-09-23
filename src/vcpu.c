#include "vcpu.h"

#include "ablation.h"
#include "boot/acpi.h"
#include "kvm.h"
#include "metrics.h"
#include "serial.h"

#include <errno.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * vCPU 模块：创建 KVM vCPU、按 Linux x86 32 位启动协议设置初始寄存器，
 * 并在 KVM_RUN 循环中处理客户机退出（VM exit）。
 *
 * 每个 vCPU 对应一个由 KVM_CREATE_VCPU 返回的文件描述符，以及一块
 * 通过 mmap 与内核共享的 struct kvm_run。KVM_RUN 返回后，内核把退出
 * 原因和相关数据（端口 I/O、MMIO 等）写进 kvm_run；VMM 处理完后再次
 * 调用 KVM_RUN，内核会把 VMM 填写的结果（例如 IN 指令读到的值）
 * 交还给客户机并继续执行。
 */

/*
 * 从 /dev/kvm 查询宿主机支持的 CPUID 项，再安装到当前 vCPU；
 * 否则客户机执行 CPUID 时得不到与 KVM 能力匹配的特性列表。
 *
 * KVM 新建的 vCPU 默认 CPUID 表几乎为空，Linux 早期代码会用 CPUID 检查
 * 长模式、PAE 等特性；缺少这些信息时内核可能拒绝启动或走错误路径。
 * 这里直接把 KVM 支持的全集原样交给客户机，不做裁剪。
 *
 * 成功返回 0；失败返回 -1，并已打印错误信息。
 */
static int vcpu_setup_cpuid(struct vcpu *vcpu, int system_fd, unsigned apic_id)
{
    /*
     * CPUID 条目数量随 CPU 型号和内核版本变化，事先无法确定。
     * 从 256 开始尝试，最多扩到 1024，防止异常返回值导致无限分配。
     */
    size_t capacity = 256;
    while (capacity <= 1024) {
        /* kvm_cpuid2 的 entries 是变长数组，nent 告诉内核可容纳的条目数。 */
        size_t bytes = sizeof(struct kvm_cpuid2) +
                       capacity * sizeof(struct kvm_cpuid_entry2);
        struct kvm_cpuid2 *cpuid = calloc(1, bytes);
        if (!cpuid) {
            perror("allocate KVM CPUID table");
            return -1;
        }
        cpuid->nent = (uint32_t)capacity;
        /*
         * KVM_GET_SUPPORTED_CPUID 作用在 /dev/kvm（system fd）上，描述的是
         * 宿主机 + KVM 能向客户机提供的能力；成功时内核把 nent 改成实际
         * 条目数。KVM_SET_CPUID2 作用在 vCPU fd 上，把这张表安装给该 vCPU，
         * 必须在第一次 KVM_RUN 之前完成。
         */
        if (ioctl(system_fd, KVM_GET_SUPPORTED_CPUID, cpuid) == 0) {
            /*
             * TSC-deadline 定时器由内核 LAPIC 模拟提供，但不在
             * GET_SUPPORTED_CPUID 的结果里，需要按 KVM_CAP_TSC_DEADLINE_TIMER
             * 自行打开 CPUID.1:ECX bit 24。ACPI 硬件精简模式下没有 PIT 可用来
             * 校准 LAPIC 定时器，Linux 依赖它作为时钟事件设备。
             */
            int tsc_deadline = ioctl(system_fd, KVM_CHECK_EXTENSION,
                                     KVM_CAP_TSC_DEADLINE_TIMER) > 0;
            /*
             * 宿主机 CPUID 里的 APIC ID 是宿主机当前 CPU 的编号，要换成本 vCPU
             * 的 ID（LAPIC 复位后等于 vCPU id），否则 Linux 会报 APIC ID 不一致：
             * leaf 1 的 EBX[31:24]，以及 x2APIC 拓扑 leaf 0xb/0x1f 的 EDX。
             */
            for (uint32_t i = 0; i < cpuid->nent; ++i) {
                struct kvm_cpuid_entry2 *entry = &cpuid->entries[i];
                if (entry->function == 1) {
                    entry->ebx = (entry->ebx & 0x00ffffffu) | (apic_id << 24);
                    if (tsc_deadline)
                        entry->ecx |= 1u << 24;
                } else if (entry->function == 0xb || entry->function == 0x1f) {
                    entry->edx = apic_id;
                }
            }
            int result = ioctl(vcpu->fd, KVM_SET_CPUID2, cpuid);
            if (result < 0)
                perror("KVM_SET_CPUID2");
            free(cpuid);
            return result;
        }
        /* free() 可能改写 errno，所以先保存失败原因和内核回填的 nent。 */
        int saved_errno = errno;
        size_t required = cpuid->nent;
        free(cpuid);
        if (saved_errno != E2BIG) {
            errno = saved_errno;
            perror("KVM_GET_SUPPORTED_CPUID");
            return -1;
        }
        /* E2BIG 表示数组装不下：优先用内核报告的需求量，否则翻倍重试。 */
        capacity = required > capacity ? required : capacity * 2;
    }
    fprintf(stderr, "KVM CPUID table exceeds 1024 entries\n");
    return -1;
}

/*
 * 在 kvm->vm_fd 对应的虚拟机里创建编号为 id 的 vCPU，映射它的 kvm_run，
 * 并安装 CPUID 表。
 *
 * 失败时可能已经打开了 vCPU fd 或建立了映射，但这些资源都已记录在
 * *vcpu 中；调用者无论成败都应调用 vcpu_destroy() 统一释放。
 */
int vcpu_init(struct vcpu *vcpu, const struct kvm_context *kvm, unsigned id)
{
    /* 先置为“无资源”状态，保证任意失败路径下 vcpu_destroy() 都安全。 */
    vcpu->fd = -1;
    vcpu->run = NULL;
    vcpu->run_size = 0;

    /*
     * kvm_run 映射的大小由内核决定：除 struct kvm_run 本身外，后面还可能
     * 跟着端口 I/O 数据缓冲区等额外页面。所以必须询问 /dev/kvm，而不能
     * 直接用 sizeof(struct kvm_run)；返回值小于结构体本身说明环境异常。
     */
    int size = ioctl(kvm->system_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (size < (int)sizeof(struct kvm_run)) {
        if (size < 0)
            perror("KVM_GET_VCPU_MMAP_SIZE");
        else
            fprintf(stderr, "KVM run mapping is too small\n");
        return -1;
    }
    vcpu->fd = ioctl(kvm->vm_fd, KVM_CREATE_VCPU, id);
    if (vcpu->fd < 0) {
        perror("KVM_CREATE_VCPU");
        return -1;
    }
    /*
     * 映射 vCPU fd 偏移 0 处的共享区域。必须用 MAP_SHARED：内核在每次
     * VM exit 时写入退出信息，VMM 写入的 I/O 结果也要让内核看到。
     */
    vcpu->run_size = (size_t)size;
    void *mapping = mmap(NULL, vcpu->run_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, vcpu->fd, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap kvm_run");
        return -1;
    }
    vcpu->run = mapping;
    /* no_cpuid 消融项保留 KVM 的默认 CPUID，用来观察缺少 CPUID 时的失败点。 */
    if (SVMM_ABLATE_CPUID_ENABLED)
        return 0;
    return vcpu_setup_cpuid(vcpu, kvm->system_fd, id);
}

/*
 * 按 Linux x86 32 位启动协议设置 vCPU 的初始状态，使第一次 KVM_RUN
 * 直接从 kernel_addr（压缩内核的 code32_start）开始执行。
 *
 * 协议要求进入 32 位入口时：
 *   - 处于保护模式、未开启分页；
 *   - GDT 中 __BOOT_CS(0x10) 和 __BOOT_DS(0x18) 都是基址 0、界限 4 GiB 的
 *     平坦段，CS 可执行/可读，DS/ES/SS 等可读/可写；
 *   - 关闭中断；
 *   - %esi 指向 struct boot_params（zero page）；
 *   - %ebp、%edi、%ebx 为 0。
 * 对应的 GDT 内容由 boot_linux_prepare() 写到客户机物理地址 0x500。
 */
int vcpu_setup_linux_boot(struct vcpu *vcpu, unsigned kernel_addr,
                          unsigned boot_params_addr)
{
    /*
     * no_protected_mode 消融项跳过段寄存器和控制寄存器设置，vCPU 保持
     * KVM 默认的实模式复位状态，用来观察直接跳 32 位入口会在哪里失败。
     */
    if (!SVMM_ABLATE_PROTECTED_MODE_ENABLED) {
        /*
         * 先读回 KVM 的当前值再改需要的字段，其他字段（IDT、LDT、TR、
         * APIC base 等）保持默认，避免把它们意外清成非法值。
         */
        struct kvm_sregs sregs;
        if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0) {
            perror("KVM_GET_SREGS");
            return -1;
        }
        /*
         * The 32-bit boot protocol enters the compressed kernel directly.
         *
         * kvm_segment 描述的是段寄存器的“隐藏部分”（描述符缓存）。直接
         * 写入它就相当于 CPU 已经从 GDT 装载过这个段，所以不需要执行
         * 客户机代码来加载段寄存器。各字段含义：
         *   base = 0, limit = 0xffffffff  平坦段，覆盖全部 4 GiB
         *                                 （KVM 使用字节单位的 limit）
         *   selector = 0x10               GDT index 2，RPL 0，即 __BOOT_CS
         *   type = 0xb                    代码段：可执行、可读、已访问
         *   present = 1                   段存在
         *   s = 1                         代码/数据段，而不是系统段
         *   db = 1                        默认操作数和地址宽度为 32 位
         *   g = 1                         4 KiB 粒度，与 GDT 描述符一致
         * dpl 等未列出的字段为 0，即内核特权级 ring 0。
         */
        struct kvm_segment code = {
            .base = 0,
            .limit = 0xffffffff,
            .selector = 0x10,
            .type = 0xb,
            .present = 1,
            .s = 1,
            .db = 1,
            .g = 1,
        };
        /* 数据段与代码段相同，只是 selector 为 0x18（index 3，__BOOT_DS），
         * type = 0x3 表示可读、可写、已访问的数据段。 */
        struct kvm_segment data = code;
        data.selector = 0x18;
        data.type = 0x3;
        sregs.cs = code;
        sregs.ds = data;
        sregs.es = data;
        sregs.fs = data;
        sregs.gs = data;
        sregs.ss = data;
        /*
         * GDTR 指向 boot_linux_prepare() 写入的 4 项 GDT（每项 8 字节）。
         * limit 是“最后一个有效字节的偏移”，因此是总长度减 1。内核之后
         * 重新加载段寄存器时会从这里读取描述符，所以内容必须与上面一致。
         */
        sregs.gdt.base = 0x500;
        sregs.gdt.limit = 4 * 8 - 1;
        /*
         * CR0.PE（bit 0）置 1 进入保护模式；CR0.PG（bit 31）清 0 关闭分页，
         * 此时线性地址直接等于物理地址。CR3（页表基址）、CR4（PAE 等扩展）
         * 和 EFER（含 LME 长模式使能）全部清零：进入长模式由内核自己完成。
         */
        sregs.cr0 = (sregs.cr0 | 1u) & ~(1u << 31);
        sregs.cr3 = 0;
        sregs.cr4 = 0;
        sregs.efer = 0;
        if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0) {
            perror("KVM_SET_SREGS");
            return -1;
        }
    }
    /*
     * 通用寄存器。未列出的字段由指定初始化器置 0，正好满足协议对
     * %ebp、%edi、%ebx 为 0 的要求。
     *   rip    = 32 位内核入口，即压缩内核被载入的地址
     *   rflags = 0x2：bit 1 是保留位，硬件要求恒为 1；IF = 0 表示关中断
     *   rsi    = zero page 的客户机物理地址
     *   rsp    = 0x90000：给入口代码一个位于 e820 可用区（64 KiB–1 MiB）
     *            内的临时栈，向下增长，不会碰到 0x20000 处的命令行。
     *            内核很快会切换到自己的栈。
     */
    struct kvm_regs regs = {
        .rip = kernel_addr,
        .rflags = 0x2,
        .rsi = boot_params_addr,
        .rsp = 0x90000,
    };
    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0) {
        perror("KVM_SET_REGS");
        return -1;
    }
    return 0;
}

/* 输出本次运行的退出指标（供消融实验脚本解析），并把 status 原样返回。 */
static int vcpu_finish(struct vcpu_run_stats *stats, const char *reason,
                       int status)
{
    metric_exit(stderr, SVMM_VARIANT_NAME, reason, stats);
    return status;
}

/*
 * 处理与电源相关的平台端口。返回 1 表示客户机请求关机或复位，
 * 0 表示端口已处理，-1 表示不是这些端口。
 *
 *   ACPI 睡眠控制（FADT SLEEP_CONTROL_REG）：SLP_EN 且 SLP_TYP = 5 为 S5 关机
 *   ACPI 睡眠状态（FADT SLEEP_STATUS_REG）：读恒为 0
 *   0xcf9 复位控制（FADT RESET_REG）：bit 2 置位表示复位 CPU
 *   0x64 i8042 命令端口：0xfe 是经典的“脉冲复位线”命令
 */
static int platform_handle_io(const struct kvm_run *run, uint8_t *data,
                              size_t size, const char **event)
{
    uint16_t port = run->io.port;
    if (run->io.direction == KVM_EXIT_IO_IN) {
        if (port != ACPI_SLEEP_CONTROL_PORT && port != ACPI_SLEEP_STATUS_PORT &&
            port != ACPI_RESET_PORT)
            return -1;
        memset(data, 0, size);
        return 0;
    }
    uint8_t value = data[0];
    switch (port) {
    case ACPI_SLEEP_CONTROL_PORT:
        if ((value & ACPI_SLEEP_ENABLE) &&
            ((value >> 2) & 0x7) == ACPI_SLEEP_TYPE_S5) {
            *event = "poweroff";
            return 1;
        }
        return 0;
    case ACPI_SLEEP_STATUS_PORT:
        return 0;
    case ACPI_RESET_PORT:
        if (value & 0x04) {
            *event = "reset";
            return 1;
        }
        return 0;
    case 0x64:
        if (value == 0xfe) {
            *event = "reset";
            return 1;
        }
        return -1;
    default:
        return -1;
    }
}

/*
 * 请求 vcpu_run() 尽快返回，可以从其他线程或信号处理函数中调用。
 * immediate_exit 让下一次（或正在进入的）KVM_RUN 立刻以 EINTR 返回，
 * 信号则打断已经在客户机里运行的 KVM_RUN。
 */
void vcpu_request_stop(struct vcpu *vcpu)
{
    vcpu->stop_requested = 1;
    if (vcpu->run)
        vcpu->run->immediate_exit = 1;
    if (vcpu->thread_valid)
        pthread_kill(vcpu->thread, SIGUSR1);
}

/*
 * 反复进入客户机，直到客户机停机或出错。
 *
 * 每次 KVM_RUN 返回代表一次 VM exit：客户机执行了 KVM 无法在内核内
 * 自行处理、需要用户态 VMM 参与的操作。中断控制器、PIT 和 HLT 由 KVM
 * 在内核中处理；这里只模拟 COM1 串口和 ACPI 关机/复位端口，其他端口
 * I/O 和 MMIO 都按“没有设备”处理后继续运行。
 *
 * 返回 0 表示客户机关机/复位、执行了用户态可见的 HLT，或宿主请求停止
 * （vcpu_request_stop）；-1 表示 KVM 出错、客户机三重故障或遇到无法
 * 处理的退出。stats 记录退出次数、串口退出次数和最后一次
 * 退出原因，无论哪种结果都会通过 vcpu_finish() 输出。
 */
int vcpu_run(struct vcpu *vcpu, struct serial *serial,
             struct vcpu_run_stats *stats)
{
    /* 限制“未模拟设备”日志条数，避免内核探测硬件时刷屏。 */
    unsigned ignored_io = 0;
    *stats = (struct vcpu_run_stats){ 0 };
    vcpu->thread = pthread_self();
    vcpu->thread_valid = 1;
    for (;;) {
        if (vcpu->stop_requested) {
            fprintf(stderr, "INFO: VMM stopped by host request\n");
            return vcpu_finish(stats, "host_stop", 0);
        }
        /*
         * KVM_RUN 会阻塞，直到发生需要用户态处理的退出。被信号打断时
         * 返回 EINTR，这不是客户机错误：若是停止请求就退出，否则重新进入。
         */
        if (ioctl(vcpu->fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR) {
                if (!vcpu->stop_requested && vcpu->run)
                    vcpu->run->immediate_exit = 0;
                continue;
            }
            perror("KVM_RUN");
            return vcpu_finish(stats, "kvm_error", -1);
        }
        /* entered 标记客户机至少成功运行过一次，用于区分“启动前失败”。 */
        struct kvm_run *run = vcpu->run;
        stats->entered = 1;
        ++stats->exits;
        stats->exit_reason = run->exit_reason;
        switch (run->exit_reason) {
        /*
         * 客户机执行了 HLT。使用内核 irqchip 时 KVM 自己等待中断，不会
         * 产生这个退出；若仍然收到，说明 vCPU 不会再被唤醒，把它视为
         * 客户机运行结束，打印 RIP 便于定位停在哪里。
         */
        case KVM_EXIT_HLT: {
            struct kvm_regs regs;
            if (ioctl(vcpu->fd, KVM_GET_REGS, &regs) == 0)
                fprintf(stderr, "INFO: guest halted at RIP=0x%llx exits=%lu serial=%lu\n",
                        (unsigned long long)regs.rip, stats->exits,
                        stats->serial_exits);
            else
                fprintf(stderr, "INFO: guest halted\n");
            return vcpu_finish(stats, "hlt", 0);
        }
        /*
         * 在真实硬件上会导致 CPU 复位的事件，最常见的是三重故障：
         * 处理异常时又发生异常，且处理双重故障时再次失败。通常意味着
         * GDT/IDT/页表或初始寄存器状态有误。
         */
        case KVM_EXIT_SHUTDOWN:
            fprintf(stderr, "ERROR: guest shutdown (possible triple fault)\n");
            return vcpu_finish(stats, "shutdown", -1);
        /*
         * 客户机执行了 IN/OUT（或 INS/OUTS 串操作）。kvm_run->io 描述：
         *   port        端口号
         *   size        每次访问的宽度：1、2 或 4 字节
         *   count       重复次数，REP INS/OUTS 时可能大于 1
         *   direction   KVM_EXIT_IO_IN 或 KVM_EXIT_IO_OUT
         *   data_offset 数据缓冲区相对 kvm_run 映射起点的字节偏移
         * OUT 时缓冲区里是客户机写出的数据；IN 时由 VMM 填写，下一次
         * KVM_RUN 时内核再把它写回客户机的寄存器或内存。
         */
        case KVM_EXIT_IO: {
            if (run->io.size != 1 && run->io.size != 2 && run->io.size != 4) {
                fprintf(stderr, "invalid I/O width: %u\n", run->io.size);
                return vcpu_finish(stats, "invalid_io_width", -1);
            }
            /*
             * 数据缓冲区位于共享映射内部。访问前确认 [offset, offset + size)
             * 完全落在映射范围里；用减法比较以避免 offset + size 溢出。
             */
            size_t data_size = (size_t)run->io.size * run->io.count;
            size_t offset = (size_t)run->io.data_offset;
            if (offset > vcpu->run_size || data_size > vcpu->run_size - offset) {
                fprintf(stderr, "KVM I/O data is outside the run mapping\n");
                return vcpu_finish(stats, "invalid_io_data", -1);
            }
            uint8_t *data = (uint8_t *)run + offset;
            const char *event = NULL;
            int platform = platform_handle_io(run, data, data_size, &event);
            if (platform > 0) {
                fprintf(stderr, "INFO: guest requested %s\n", event);
                return vcpu_finish(stats, event, 0);
            }
            if (platform == 0)
                break;
            /* no_uart 消融项让串口端口也走“未模拟设备”分支。 */
            if (!SVMM_ABLATE_UART_ENABLED && serial_handles_port(run->io.port)) {
                ++stats->serial_exits;
                if (run->io.direction == KVM_EXIT_IO_OUT) {
                    /* 客户机写串口：交给 UART 模型，THR 写入会输出到宿主机。 */
                    if (serial_handle_out(serial, run->io.port, data, data_size) < 0) {
                        perror("serial output");
                        return vcpu_finish(stats, "serial_error", -1);
                    }
                } else {
                    /*
                     * 客户机读串口：UART 寄存器都是 8 位，每次访问只填每个
                     * 元素的第一个字节，其余字节先清零，避免返回上次的残留值。
                     */
                    memset(data, 0, data_size);
                    for (size_t i = 0; i < run->io.count; ++i) {
                        if (serial_handle_in(serial, run->io.port,
                                             data + i * run->io.size) < 0)
                            return vcpu_finish(stats, "serial_error", -1);
                    }
                }
            } else {
                /*
                 * 未模拟的端口：写入直接丢弃；读取返回全 1，模拟 ISA 总线上
                 * 没有设备应答时的浮空值，内核探测到 0xff 通常会认为设备不存在。
                 */
                if (run->io.direction == KVM_EXIT_IO_IN)
                    memset(data, 0xff, data_size);
                if (ignored_io++ < 8)
                    fprintf(stderr, "INFO: unmodeled I/O port=0x%x direction=%u\n",
                            run->io.port, run->io.direction);
            }
            break;
        }
        /*
         * 客户机访问了未映射为 RAM 的物理地址（例如 LAPIC、IOAPIC、HPET
         * 或超出内存范围的地址）。mmio.data 最多 8 字节，len 为访问宽度。
         * 当前没有 MMIO 设备：写入丢弃，读取同样返回全 1。
         */
        case KVM_EXIT_MMIO:
            if (run->mmio.len > sizeof(run->mmio.data)) {
                fprintf(stderr, "invalid MMIO width: %u\n", run->mmio.len);
                return vcpu_finish(stats, "invalid_mmio_width", -1);
            }
            if (!run->mmio.is_write)
                memset(run->mmio.data, 0xff, run->mmio.len);
            if (ignored_io++ < 8)
                fprintf(stderr, "INFO: unmodeled MMIO address=0x%llx\n",
                        (unsigned long long)run->mmio.phys_addr);
            break;
        /* 其他退出原因（如 KVM_EXIT_FAIL_ENTRY、KVM_EXIT_INTERNAL_ERROR）无法恢复。 */
        default:
            fprintf(stderr, "unexpected KVM exit reason: %u\n", run->exit_reason);
            return vcpu_finish(stats, "unexpected_exit", -1);
        }
    }
}

/*
 * 释放 vcpu_init() 获得的资源：先解除 kvm_run 映射，再关闭 vCPU fd。
 * 结束后恢复为“无资源”状态，因此对部分初始化或重复调用都是安全的。
 */
void vcpu_destroy(struct vcpu *vcpu)
{
    if (vcpu->run)
        munmap(vcpu->run, vcpu->run_size);
    if (vcpu->fd >= 0)
        close(vcpu->fd);
    vcpu->fd = -1;
    vcpu->run = NULL;
    vcpu->run_size = 0;
}

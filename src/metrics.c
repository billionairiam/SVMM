#include "metrics.h"

#include "boot/acpi.h"
#include "serial.h"
#include "vcpu.h"

#include <linux/kvm.h>
#include <stdlib.h>

void metric_stage(FILE *stream, const char *variant, const char *stage,
                  size_t guest_memory_bytes)
{
    fprintf(stream,
            "SVMM_METRIC variant=%s stage=%s guest_memory_bytes=%zu\n",
            variant, stage, guest_memory_bytes);
}

void metric_exit(FILE *stream, const char *variant, const char *reason,
                 const struct vcpu_run_stats *stats)
{
    fprintf(stream,
            "SVMM_METRIC variant=%s stage=vcpu_exit reason=%s entered=%d "
            "exits=%lu serial_exits=%lu\n",
            variant, reason, stats->entered, stats->exits,
            stats->serial_exits);
}

static const char *exit_reason_name(unsigned reason)
{
    switch (reason) {
    case KVM_EXIT_UNKNOWN: return "KVM_EXIT_UNKNOWN";
    case KVM_EXIT_EXCEPTION: return "KVM_EXIT_EXCEPTION";
    case KVM_EXIT_IO: return "KVM_EXIT_IO";
    case KVM_EXIT_HYPERCALL: return "KVM_EXIT_HYPERCALL";
    case KVM_EXIT_DEBUG: return "KVM_EXIT_DEBUG";
    case KVM_EXIT_HLT: return "KVM_EXIT_HLT";
    case KVM_EXIT_MMIO: return "KVM_EXIT_MMIO";
    case KVM_EXIT_IRQ_WINDOW_OPEN: return "KVM_EXIT_IRQ_WINDOW_OPEN";
    case KVM_EXIT_SHUTDOWN: return "KVM_EXIT_SHUTDOWN";
    case KVM_EXIT_FAIL_ENTRY: return "KVM_EXIT_FAIL_ENTRY";
    case KVM_EXIT_INTR: return "KVM_EXIT_INTR";
    case KVM_EXIT_INTERNAL_ERROR: return "KVM_EXIT_INTERNAL_ERROR";
    case KVM_EXIT_SYSTEM_EVENT: return "KVM_EXIT_SYSTEM_EVENT";
    default: return NULL;
    }
}

/*
 * 给常见的 PC 端口起个名字，方便看出客户机在和哪个“设备”打交道。
 * 标注 not emulated 的端口没有任何设备模型：读返回 0xff，写被丢弃。
 */
static const char *io_port_name(uint16_t port, uint8_t direction)
{
    int in = direction == KVM_EXIT_IO_IN;
    if (serial_handles_port(port)) {
        static const char *const com1_in[] = {
            "COM1 RBR/DLL (receive byte)", "COM1 IER/DLM",
            "COM1 IIR (interrupt id)", "COM1 LCR (line control)",
            "COM1 MCR (modem control)", "COM1 LSR (line status)",
            "COM1 MSR (modem status)", "COM1 SCR (scratch)",
        };
        static const char *const com1_out[] = {
            "COM1 THR/DLL (transmit byte)", "COM1 IER/DLM",
            "COM1 FCR (FIFO control)", "COM1 LCR (line control)",
            "COM1 MCR (modem control)", "COM1 LSR (read-only)",
            "COM1 MSR (read-only)", "COM1 SCR (scratch)",
        };
        return (in ? com1_in : com1_out)[port - COM1_PORT];
    }
    switch (port) {
    case ACPI_SLEEP_CONTROL_PORT: return "ACPI sleep control";
    case ACPI_SLEEP_STATUS_PORT: return "ACPI sleep status";
    case ACPI_RESET_PORT: return "ACPI reset register";
    case 0xcf8: return "PCI config address (not emulated)";
    case 0xcfa: case 0xcfb: return "PCI config mechanism probe (not emulated)";
    case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
        return "PCI config data (not emulated)";
    case 0x60: return "i8042 data (not emulated)";
    case 0x64: return "i8042 command/status (not emulated)";
    case 0x70: case 0x71: return "CMOS/RTC (not emulated)";
    case 0x80: return "POST/delay port (not emulated)";
    case 0x87: return "DMA page register (not emulated)";
    default: break;
    }
    if (port >= 0x2f8 && port <= 0x2ff)
        return "COM2 (not emulated)";
    if (port >= 0x3e8 && port <= 0x3ef)
        return "COM3 (not emulated)";
    if (port >= 0x2e8 && port <= 0x2ef)
        return "COM4 (not emulated)";
    return "(not emulated)";
}

static int compare_io_ports(const void *a, const void *b)
{
    const struct vcpu_io_port_stat *left = a;
    const struct vcpu_io_port_stat *right = b;
    if (left->exits != right->exits)
        return left->exits < right->exits ? 1 : -1;
    if (left->port != right->port)
        return left->port < right->port ? -1 : 1;
    return (int)left->direction - (int)right->direction;
}

void metric_exit_breakdown(FILE *stream, const struct vcpu_run_stats *stats)
{
    if (!stats->exits)
        return;
    fprintf(stream,
            "VM exits handled by the VMM: %lu "
            "(exits KVM handles in the kernel are not counted)\n",
            stats->exits);
    fprintf(stream, "  %-26s %10s %8s\n", "exit reason", "exits", "share");
    for (unsigned reason = 0; reason < VCPU_EXIT_REASON_SLOTS; ++reason) {
        unsigned long count = stats->reason_exits[reason];
        if (!count)
            continue;
        const char *name = exit_reason_name(reason);
        char fallback[32];
        if (!name) {
            snprintf(fallback, sizeof(fallback), "exit reason %u%s", reason,
                     reason == VCPU_EXIT_REASON_SLOTS - 1 ? "+" : "");
            name = fallback;
        }
        fprintf(stream, "  %-26s %10lu %7.2f%%\n", name, count,
                100.0 * (double)count / (double)stats->exits);
    }

    if (!stats->io_port_count)
        return;
    struct vcpu_io_port_stat ports[VCPU_IO_PORT_SLOTS];
    unsigned count = stats->io_port_count;
    for (unsigned i = 0; i < count; ++i)
        ports[i] = stats->io_ports[i];
    qsort(ports, count, sizeof(ports[0]), compare_io_ports);
    fprintf(stream, "  I/O exits by port (one REP INS/OUTS counts once):\n");
    fprintf(stream, "  %-8s %-4s %10s  %s\n", "port", "dir", "exits", "device");
    for (unsigned i = 0; i < count; ++i)
        fprintf(stream, "  0x%04x   %-4s %10lu  %s\n", ports[i].port,
                ports[i].direction == KVM_EXIT_IO_IN ? "in" : "out",
                ports[i].exits, io_port_name(ports[i].port, ports[i].direction));
    if (stats->io_other_exits)
        fprintf(stream, "  %-13s %10lu  (port table full)\n", "other ports",
                stats->io_other_exits);
}

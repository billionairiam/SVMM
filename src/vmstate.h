#ifndef VMSTATE_H
#define VMSTATE_H

#include "serial.h"
#include "virtio/mmio.h"

#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Stage 11：热快照（warm snapshot）里“除了内存以外”的全部机器状态。
 *
 * 冷快照（Stage 10）只存磁盘，恢复时客户机重新 boot。热快照要让客户机从
 * 暂停的那条指令继续执行，所以还要存下 KVM 里看不见的那部分机器：
 *
 *   VM 级（vm_fd）       中断控制器（两片 8259 PIC + I/O APIC）、PIT、kvmclock
 *   vCPU 级（vcpu_fd）   通用寄存器、段/控制寄存器、FPU/SSE/AVX（XSAVE）、XCR0、
 *                        调试寄存器、LAPIC、MSR（含 TSC）、挂起的异常/中断/NMI、
 *                        运行状态（RUNNABLE / HALTED）、CPUID、TSC 频率
 *   设备（VMM 用户态）   16550 串口寄存器和接收队列、每个 virtio-mmio 设备的
 *                        传输层寄存器和 virtqueue 位置（last_avail / next_used）
 *
 * virtqueue 本身（desc / avail / used 三块 ring）在客户机内存里，随内存一起保存；
 * 这里只存 device 侧那两个不在客户机内存里的计数器和 ring 的 GPA。
 *
 * 文件格式（vmstate.bin，小端，与 VMM 二进制同机使用）：
 *
 *   "SVMMVMST"  u32 version  u32 section_count
 *   section_count 个 { u32 tag, u32 length, payload[length] }
 *
 * payload 是 KVM 结构体的原样字节，或下面 *_state 的定长结构。解码是 fail-closed
 * 的：未知 tag、重复、缺少必需的 section、长度不对、文件尾多余字节、计数越界
 * 都拒绝，不会“尽量恢复”。
 */

#define VMSTATE_MAGIC "SVMMVMST"
#define VMSTATE_VERSION 1u
#define VMSTATE_MAX_MSRS 256u
#define VMSTATE_MAX_CPUID 256u

/* section tag。数值是文件格式的一部分，只能追加，不能改。 */
enum vmstate_tag {
    VMSTATE_TAG_PIC_MASTER = 1,
    VMSTATE_TAG_PIC_SLAVE = 2,
    VMSTATE_TAG_IOAPIC = 3,
    VMSTATE_TAG_PIT = 4,
    VMSTATE_TAG_CLOCK = 5,
    VMSTATE_TAG_CPUID = 10,
    VMSTATE_TAG_TSC_KHZ = 11,
    VMSTATE_TAG_MP_STATE = 12,
    VMSTATE_TAG_REGS = 13,
    VMSTATE_TAG_SREGS = 14,
    VMSTATE_TAG_XSAVE = 15,
    VMSTATE_TAG_XCRS = 16,
    VMSTATE_TAG_DEBUGREGS = 17,
    VMSTATE_TAG_LAPIC = 18,
    VMSTATE_TAG_MSRS = 19,
    VMSTATE_TAG_EVENTS = 20,
    VMSTATE_TAG_SERIAL = 30,
    VMSTATE_TAG_VIRTIO_BLK = 31,
    VMSTATE_TAG_VIRTIO_NET = 32,
};

/* 设备是否存在。恢复时必须和这次 run 的 --disk / --tap 一致。 */
#define VMSTATE_DEV_BLK 0x1u
#define VMSTATE_DEV_NET 0x2u

struct vmstate_vm {
    struct kvm_irqchip pic_master;
    struct kvm_irqchip pic_slave;
    struct kvm_irqchip ioapic;
    struct kvm_pit_state2 pit;
    struct kvm_clock_data clock;
};

struct vmstate_cpu {
    uint32_t ncpuid;
    struct kvm_cpuid_entry2 cpuid[VMSTATE_MAX_CPUID];
    uint32_t tsc_khz;
    struct kvm_mp_state mp_state;
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct kvm_xsave xsave;
    struct kvm_xcrs xcrs;
    struct kvm_debugregs debugregs;
    struct kvm_lapic_state lapic;
    uint32_t nmsrs;
    struct kvm_msr_entry msrs[VMSTATE_MAX_MSRS];
    struct kvm_vcpu_events events;
};

struct vmstate {
    struct vmstate_vm vm;
    struct vmstate_cpu cpu;
    struct serial_state serial;
    uint32_t devices; /* VMSTATE_DEV_* */
    struct virtio_mmio_state blk;
    struct virtio_mmio_state net;
};

struct kvm_context;
struct vcpu;

/*
 * 读出 VM 和 vCPU 的 KVM 状态（不含设备）。调用前 vCPU 必须已经停在 KVM_RUN
 * 之外并经过 vcpu_quiesce()，否则挂起的 PIO/MMIO 结果不在读出的寄存器里。
 */
int vmstate_capture(struct vmstate *st, const struct kvm_context *kvm,
                    const struct vcpu *vcpu);

/*
 * 把 KVM 状态写回一台刚创建的 VM（已有 irqchip、PIT）和刚 vcpu_init() 的 vCPU。
 * TSC 频率或 CPUID 和保存时不同（换了宿主机 / KVM 版本）就拒绝，因为客户机内核
 * 早就按旧的 CPUID 选好了代码路径。TSC 最先比较；CPUID 最后比较，因为其中几位由
 * KVM 按 CR4/XCR0 等实时改写（见 vmstate.c）。任何一个 ioctl 失败都返回 -1；
 * 失败时这台 VM 只能丢弃。
 */
int vmstate_apply(const struct vmstate *st, const struct kvm_context *kvm,
                  const struct vcpu *vcpu);

/* 编码成 vmstate.bin 的字节；*out 由调用者 free()。 */
int vmstate_encode(const struct vmstate *st, uint8_t **out, size_t *len);

/* 严格解码（见文件头）。失败时打印 "ERROR: vmstate: ..." 并返回 -1。 */
int vmstate_decode(struct vmstate *st, const uint8_t *buf, size_t len);

#endif

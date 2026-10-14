#ifndef KVM_CONTEXT_H
#define KVM_CONTEXT_H

/*
 * KVM 的 API 是三层文件描述符，每层用 ioctl 操作，上一层创建下一层：
 *
 *   /dev/kvm   (system_fd)  整个 KVM 模块：查询 API 版本、能力（KVM_CHECK_EXTENSION）、
 *      │                    CPUID 支持表、kvm_run 映射大小；KVM_CREATE_VM 创建虚拟机
 *      ▼
 *   VM fd      (vm_fd)      一台虚拟机：内存槽（KVM_SET_USER_MEMORY_REGION）、
 *      │                    中断控制器（KVM_CREATE_IRQCHIP）、中断线（KVM_IRQ_LINE）；
 *      ▼                    KVM_CREATE_VCPU 创建 vCPU
 *   vCPU fd    (vcpu.c)     一个虚拟 CPU：寄存器（KVM_SET_REGS/SREGS）、CPUID、
 *                           KVM_RUN 进入客户机
 *
 * 这里只管前两层；vCPU 在 vcpu.c，内存在 memory.c。
 */
struct kvm_context {
    int system_fd;
    int vm_fd;
};

/* 打开 /dev/kvm，创建 VM，并在内核里创建中断控制器和 PIT。失败返回 -1。 */
int kvm_context_init(struct kvm_context *kvm);
/*
 * 设置一根中断线的电平，相当于设备把芯片上的 IRQ 引脚拉高（level=1）或拉低（0）。
 * irq 是 GSI（全局系统中断号）：有内核 irqchip 时，GSI 0–15 同时接到 8259 和
 * I/O APIC 的同号引脚，16–23 只接 I/O APIC。客户机内核怎么把 GSI 变成自己的
 * IRQ 号、是边沿还是电平触发，由 ACPI（MADT、DSDT）和 I/O APIC 编程决定。
 * 串口用 GSI 4，virtio-blk 用 GSI 5。
 */
int kvm_set_irq_line(const struct kvm_context *kvm, unsigned irq, int level);
void kvm_context_destroy(struct kvm_context *kvm);

#endif

#include "kvm.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int kvm_context_init(struct kvm_context *kvm)
{
    kvm->system_fd = -1;
    kvm->vm_fd = -1;
    /* 需要读写权限：通常是 root，或者属于 kvm 组。O_CLOEXEC 防止 fd 泄漏给子进程。 */
    kvm->system_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm->system_fd < 0) {
        perror("open /dev/kvm");
        return -1;
    }
    /*
     * KVM 的稳定 API 版本自 Linux 2.6.22 起一直是 12，之后的新功能都通过
     * KVM_CHECK_EXTENSION 逐项查询，而不是提升版本号。
     */
    int version = ioctl(kvm->system_fd, KVM_GET_API_VERSION, 0);
    if (version != KVM_API_VERSION) {
        if (version < 0)
            perror("KVM_GET_API_VERSION");
        else
            fprintf(stderr, "unsupported KVM API version: %d\n", version);
        return -1;
    }
    /*
     * 参数 0 是机器类型（x86 上只有默认类型）。刚创建的 VM 没有内存、没有
     * vCPU，也没有任何设备，下面和 memory.c、vcpu.c 一步步把它装起来。
     */
    kvm->vm_fd = ioctl(kvm->system_fd, KVM_CREATE_VM, 0);
    if (kvm->vm_fd < 0) {
        perror("KVM_CREATE_VM");
        return -1;
    }
    /*
     * TSS 地址只在 Intel 上没有 unrestricted guest 时用于模拟实模式；
     * 32 位保护模式入口通常用不到它，所以失败只警告，不中止启动。
     */
    if (ioctl(kvm->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000) < 0)
        perror("WARN: KVM_SET_TSS_ADDR");
    /*
     * 进入用户空间后，Linux 需要定时器中断来调度进程，串口驱动也需要
     * IRQ 4 才能收发字符。由 KVM 在内核里模拟 LAPIC、IOAPIC 和两片 8259，
     * 必须在创建 vCPU 之前完成；有了它，客户机的 HLT 也由 KVM 处理，
     * 等到下一个中断再唤醒 vCPU，而不是退出到 VMM。
     */
    if (ioctl(kvm->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
        perror("KVM_CREATE_IRQCHIP");
        return -1;
    }
    /* 8254 PIT 同样放在内核里；SPEAKER_DUMMY 顺带吸收 0x61 端口的访问。 */
    struct kvm_pit_config pit = { .flags = KVM_PIT_SPEAKER_DUMMY };
    if (ioctl(kvm->vm_fd, KVM_CREATE_PIT2, &pit) < 0) {
        perror("KVM_CREATE_PIT2");
        return -1;
    }
    return 0;
}

/*
 * 由设备模型调用（串口、virtio-blk），可能在 vCPU 线程，也可能在控制台线程。
 * KVM 内部对 irqchip 加锁，所以不同线程同时调用是安全的。
 *
 * 电平触发的设备要“状态变化才调用”：需要中断时拉高并保持，客户机处理完
 * （读走数据、写 ACK 寄存器）后再拉低。如果一直保持高电平，I/O APIC 在 EOI 后
 * 会再次投递，客户机就陷入中断风暴。
 */
int kvm_set_irq_line(const struct kvm_context *kvm, unsigned irq, int level)
{
    struct kvm_irq_level line = { .irq = irq, .level = level ? 1 : 0 };
    if (ioctl(kvm->vm_fd, KVM_IRQ_LINE, &line) < 0) {
        perror("KVM_IRQ_LINE");
        return -1;
    }
    return 0;
}

void kvm_context_destroy(struct kvm_context *kvm)
{
    if (kvm->vm_fd >= 0)
        close(kvm->vm_fd);
    if (kvm->system_fd >= 0)
        close(kvm->system_fd);
    kvm->vm_fd = -1;
    kvm->system_fd = -1;
}

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
    kvm->system_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm->system_fd < 0) {
        perror("open /dev/kvm");
        return -1;
    }
    int version = ioctl(kvm->system_fd, KVM_GET_API_VERSION, 0);
    if (version != KVM_API_VERSION) {
        if (version < 0)
            perror("KVM_GET_API_VERSION");
        else
            fprintf(stderr, "unsupported KVM API version: %d\n", version);
        return -1;
    }
    kvm->vm_fd = ioctl(kvm->system_fd, KVM_CREATE_VM, 0);
    if (kvm->vm_fd < 0) {
        perror("KVM_CREATE_VM");
        return -1;
    }
    // 在 Intel CPU 上运行实模式客户机时，KVM 需要 4 KiB 的客户机物理内存来存放任务状态段（Task State Segment，TSS）。
    // 这是因为当客户机执行 hlt 或触发某些异常时，KVM 内部会切换到 32 位保护模式来处理这些事件，而 TSS 是保护模式切换的必要结构。
    // VMM 必须通过 KVM_SET_TSS_ADDR 告诉 KVM 这块内存的位置。地址要求：
    // 4 KiB 对齐
    // 在 4 GiB 以下
    // 不与已注册的客户机内存槽位重叠
    // 常用的约定地址是 0xfffbd000（QEMU/kvmtool 也使用此地址），位于 32 位地址空间的顶端附近，远离我们的 64 KiB 客户机 RAM（0x0000–0xFFFF）。
    // AMD CPU 不需要此 ioctl，但为兼容性考虑，应在所有 x86 KVM 环境中调用。
    if (ioctl(kvm->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000) < 0) {
        perror("KVM_SET_TSS_ADDR");
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

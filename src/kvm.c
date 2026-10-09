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
        int open_errno = errno;
        perror("open /dev/kvm");
        if (open_errno == ENOENT)
            fprintf(stderr, "hint: load kvm_intel/kvm_amd and check that CPU virtualization is enabled\n");
        else if (open_errno == EACCES || open_errno == EPERM)
            fprintf(stderr, "hint: add the user to the kvm group (usermod -aG kvm \"$USER\") and log in again\n");
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
    // KVM_SET_TSS_ADDR 指定一块 3 页（12 KiB）的客户机物理地址区域，供 Intel VMX 下的 KVM 内部使用。
    // 当 CPU 不支持 unrestricted guest 时，VMX 无法直接运行实模式，KVM 会借助 vm86（虚拟 8086）模式模拟实模式，
    // 而 vm86 需要一个 TSS（含中断重定向位图和 I/O 位图），这块区域就用来存放它。
    // 支持 unrestricted guest 的 CPU 实际不会用到它，但仍应设置。地址要求：
    // 在 4 GiB 以下
    // 不与任何内存槽位或 MMIO 地址重叠
    // 0xfffbd000 是 kvmtool 使用的约定地址（QEMU 用 0xfeffd000），占用 0xfffbd000–0xfffbffff，远离我们的 64 KiB 客户机 RAM（0x0000–0xFFFF）。
    // AMD SVM 可直接运行实模式，此 ioctl 在 AMD 上是空操作，因此在所有 x86 KVM 上统一调用即可。
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

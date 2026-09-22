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

#include "vcpu.h"

#include "kvm.h"

#include <errno.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int vcpu_init(struct vcpu *vcpu, const struct kvm_context *kvm, unsigned id)
{
    vcpu->fd = -1;
    vcpu->run = NULL;
    vcpu->run_size = 0;

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
    vcpu->run_size = (size_t)size;
    void *mapping = mmap(NULL, vcpu->run_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, vcpu->fd, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap kvm_run");
        return -1;
    }
    vcpu->run = mapping;
    return 0;
}

int vcpu_setup_regs(struct vcpu *vcpu)
{
    struct kvm_sregs sregs;
    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        return -1;
    }
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        return -1;
    }
    struct kvm_regs regs = {
        .rip = 0,
        .rflags = 0x2,
    };
    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0) {
        perror("KVM_SET_REGS");
        return -1;
    }
    return 0;
}

int vcpu_run(struct vcpu *vcpu, int out_fd)
{
    for (;;) {
        if (ioctl(vcpu->fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR)
                continue;
            perror("KVM_RUN");
            return -1;
        }
        struct kvm_run *run = vcpu->run;
        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            return 0;
        case KVM_EXIT_IO: {
            if (run->io.direction != KVM_EXIT_IO_OUT || run->io.port != 0xe9 ||
                run->io.size != 1 || run->io.count != 1) {
                fprintf(stderr, "unsupported I/O direction=%u port=0x%x size=%u count=%u\n",
                        run->io.direction, run->io.port,
                        run->io.size, run->io.count);
                return -1;
            }
            size_t offset = (size_t)run->io.data_offset;
            if (offset >= vcpu->run_size) {
                fprintf(stderr, "KVM I/O data is outside the run mapping\n");
                return -1;
            }
            const uint8_t *data = (const uint8_t *)run + offset;
            ssize_t written;
            do {
                written = write(out_fd, data, 1);
            } while (written < 0 && errno == EINTR);
            if (written != 1) {
                if (written == 0)
                    errno = EIO;
                perror("debug output");
                return -1;
            }
            break;
        }
        default:
            fprintf(stderr, "unexpected KVM exit reason: %u\n", run->exit_reason);
            return -1;
        }
    }
}

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

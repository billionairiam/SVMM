#include "vcpu.h"

#include "kvm.h"
#include "serial.h"

#include <errno.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int vcpu_setup_cpuid(struct vcpu *vcpu, int system_fd)
{
    size_t capacity = 256;
    while (capacity <= 1024) {
        size_t bytes = sizeof(struct kvm_cpuid2) +
                       capacity * sizeof(struct kvm_cpuid_entry2);
        struct kvm_cpuid2 *cpuid = calloc(1, bytes);
        if (!cpuid) {
            perror("allocate KVM CPUID table");
            return -1;
        }
        cpuid->nent = (uint32_t)capacity;
        if (ioctl(system_fd, KVM_GET_SUPPORTED_CPUID, cpuid) == 0) {
            int result = ioctl(vcpu->fd, KVM_SET_CPUID2, cpuid);
            if (result < 0)
                perror("KVM_SET_CPUID2");
            free(cpuid);
            return result;
        }
        int saved_errno = errno;
        size_t required = cpuid->nent;
        free(cpuid);
        if (saved_errno != E2BIG) {
            errno = saved_errno;
            perror("KVM_GET_SUPPORTED_CPUID");
            return -1;
        }
        capacity = required > capacity ? required : capacity * 2;
    }
    fprintf(stderr, "KVM CPUID table exceeds 1024 entries\n");
    return -1;
}

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
    return vcpu_setup_cpuid(vcpu, kvm->system_fd);
}

int vcpu_setup_linux_boot(struct vcpu *vcpu, unsigned kernel_addr,
                          unsigned boot_params_addr)
{
    struct kvm_sregs sregs;
    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        return -1;
    }
    /* The 32-bit boot protocol enters the compressed kernel directly. */
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
    struct kvm_segment data = code;
    data.selector = 0x18;
    data.type = 0x3;
    sregs.cs = code;
    sregs.ds = data;
    sregs.es = data;
    sregs.fs = data;
    sregs.gs = data;
    sregs.ss = data;
    sregs.gdt.base = 0x500;
    sregs.gdt.limit = 4 * 8 - 1;
    sregs.cr0 = (sregs.cr0 | 1u) & ~(1u << 31);
    sregs.cr3 = 0;
    sregs.cr4 = 0;
    sregs.efer = 0;
    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        return -1;
    }
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

int vcpu_run(struct vcpu *vcpu, struct serial *serial)
{
    unsigned ignored_io = 0;
    unsigned long exits = 0;
    unsigned long serial_exits = 0;
    for (;;) {
        if (ioctl(vcpu->fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR)
                continue;
            perror("KVM_RUN");
            return -1;
        }
        struct kvm_run *run = vcpu->run;
        ++exits;
        switch (run->exit_reason) {
        case KVM_EXIT_HLT: {
            struct kvm_regs regs;
            if (ioctl(vcpu->fd, KVM_GET_REGS, &regs) == 0)
                fprintf(stderr, "INFO: guest halted at RIP=0x%llx exits=%lu serial=%lu\n",
                        (unsigned long long)regs.rip, exits, serial_exits);
            else
                fprintf(stderr, "INFO: guest halted\n");
            return 0;
        }
        case KVM_EXIT_SHUTDOWN:
            fprintf(stderr, "ERROR: guest shutdown (possible triple fault)\n");
            return -1;
        case KVM_EXIT_IO: {
            if (run->io.size != 1 && run->io.size != 2 && run->io.size != 4) {
                fprintf(stderr, "invalid I/O width: %u\n", run->io.size);
                return -1;
            }
            size_t data_size = (size_t)run->io.size * run->io.count;
            size_t offset = (size_t)run->io.data_offset;
            if (offset > vcpu->run_size || data_size > vcpu->run_size - offset) {
                fprintf(stderr, "KVM I/O data is outside the run mapping\n");
                return -1;
            }
            uint8_t *data = (uint8_t *)run + offset;
            if (serial_handles_port(run->io.port)) {
                ++serial_exits;
                if (run->io.direction == KVM_EXIT_IO_OUT) {
                    if (serial_handle_out(serial, run->io.port, data, data_size) < 0) {
                        perror("serial output");
                        return -1;
                    }
                } else {
                    memset(data, 0, data_size);
                    for (size_t i = 0; i < run->io.count; ++i) {
                        if (serial_handle_in(serial, run->io.port,
                                             data + i * run->io.size) < 0)
                            return -1;
                    }
                }
            } else {
                if (run->io.direction == KVM_EXIT_IO_IN)
                    memset(data, 0xff, data_size);
                if (ignored_io++ < 8)
                    fprintf(stderr, "INFO: unmodeled I/O port=0x%x direction=%u\n",
                            run->io.port, run->io.direction);
            }
            break;
        }
        case KVM_EXIT_MMIO:
            if (run->mmio.len > sizeof(run->mmio.data)) {
                fprintf(stderr, "invalid MMIO width: %u\n", run->mmio.len);
                return -1;
            }
            if (!run->mmio.is_write)
                memset(run->mmio.data, 0xff, run->mmio.len);
            if (ignored_io++ < 8)
                fprintf(stderr, "INFO: unmodeled MMIO address=0x%llx\n",
                        (unsigned long long)run->mmio.phys_addr);
            break;
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

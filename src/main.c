#include "guest_code.h"
#include "kvm.h"
#include "memory.h"
#include "serial.h"
#include "vcpu.h"

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    struct kvm_context kvm = { .system_fd = -1, .vm_fd = -1 };
    struct guest_memory memory = { 0 };
    struct vcpu vcpu = { .fd = -1 };
    struct serial serial;
    int status = 1;

    if (kvm_context_init(&kvm) < 0)
        goto done;
    if (guest_memory_init(&memory, kvm.vm_fd) < 0)
        goto done;
    if (guest_memory_load(&memory, 0, guest_code, sizeof(guest_code)) < 0) {
        perror("load guest code");
        goto done;
    }
    if (vcpu_init(&vcpu, &kvm, 0) < 0)
        goto done;
    if (vcpu_setup_regs(&vcpu) < 0)
        goto done;
    serial_init(&serial, STDOUT_FILENO);
    if (vcpu_run(&vcpu, &serial) == 0)
        status = 0;

done:
    vcpu_destroy(&vcpu);
    guest_memory_destroy(&memory);
    kvm_context_destroy(&kvm);
    return status;
}

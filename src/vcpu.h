#ifndef VCPU_H
#define VCPU_H

#include <stddef.h>

struct kvm_context;
struct kvm_run;

struct vcpu {
    int fd;
    struct kvm_run *run;
    size_t run_size;
};

int vcpu_init(struct vcpu *vcpu, const struct kvm_context *kvm, unsigned id);
int vcpu_setup_regs(struct vcpu *vcpu);
int vcpu_run(struct vcpu *vcpu, int out_fd);
void vcpu_destroy(struct vcpu *vcpu);

#endif

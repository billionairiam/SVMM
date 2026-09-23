#ifndef KVM_CONTEXT_H
#define KVM_CONTEXT_H

struct kvm_context {
    int system_fd;
    int vm_fd;
};

int kvm_context_init(struct kvm_context *kvm);
int kvm_set_irq_line(const struct kvm_context *kvm, unsigned irq, int level);
void kvm_context_destroy(struct kvm_context *kvm);

#endif

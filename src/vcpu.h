#ifndef VCPU_H
#define VCPU_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>

struct kvm_context;
struct kvm_run;
struct serial;

struct vcpu {
    int fd;
    struct kvm_run *run;
    size_t run_size;
    /* vcpu_request_stop() 设置；可能在信号处理函数中写入。 */
    volatile sig_atomic_t stop_requested;
    pthread_t thread;
    volatile sig_atomic_t thread_valid;
};

struct vcpu_run_stats {
    unsigned long exits;
    unsigned long serial_exits;
    unsigned exit_reason;
    int entered;
};

int vcpu_init(struct vcpu *vcpu, const struct kvm_context *kvm, unsigned id);
int vcpu_setup_linux_boot(struct vcpu *vcpu, unsigned kernel_addr,
                          unsigned boot_params_addr);
int vcpu_run(struct vcpu *vcpu, struct serial *serial,
             struct vcpu_run_stats *stats);
void vcpu_request_stop(struct vcpu *vcpu);
void vcpu_destroy(struct vcpu *vcpu);

#endif

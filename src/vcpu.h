#ifndef VCPU_H
#define VCPU_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

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

/* KVM_EXIT_* 目前不到 64 种；更大的值都记在最后一格。 */
#define VCPU_EXIT_REASON_SLOTS 64u
/* 一次启动里真正被访问的端口只有几十个，表满后其余的只计总数。 */
#define VCPU_IO_PORT_SLOTS 32u

struct vcpu_io_port_stat {
    uint16_t port;
    uint8_t direction; /* KVM_EXIT_IO_IN 或 KVM_EXIT_IO_OUT */
    unsigned long exits;
};

/*
 * vcpu_run() 的统计。只包含退出到用户态 VMM 的 VM exit；KVM 在内核里
 * 自己处理掉的退出（EPT 缺页、irqchip 下的 HLT、LAPIC 定时器等）看不到。
 */
struct vcpu_run_stats {
    unsigned long exits;
    unsigned long serial_exits;
    unsigned exit_reason;
    int entered;
    /* 按 kvm_run->exit_reason 分类的退出次数。 */
    unsigned long reason_exits[VCPU_EXIT_REASON_SLOTS];
    /* 按（端口, 方向）分类的 KVM_EXIT_IO 次数，REP INS/OUTS 算一次。 */
    struct vcpu_io_port_stat io_ports[VCPU_IO_PORT_SLOTS];
    unsigned io_port_count;
    unsigned long io_other_exits;
};

int vcpu_init(struct vcpu *vcpu, const struct kvm_context *kvm, unsigned id);
int vcpu_setup_linux_boot(struct vcpu *vcpu, unsigned kernel_addr,
                          unsigned boot_params_addr);
int vcpu_run(struct vcpu *vcpu, struct serial *serial,
             struct vcpu_run_stats *stats);
void vcpu_request_stop(struct vcpu *vcpu);
void vcpu_destroy(struct vcpu *vcpu);

#endif

#ifndef VCPU_H
#define VCPU_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

struct kvm_context;
struct kvm_run;
struct serial;

/*
 * MMIO 设备回调：gpa 落在 [base, base + size) 内的 KVM_EXIT_MMIO 交给它处理，
 * offset 是相对 base 的偏移，读时把结果写进 data。
 */
struct vcpu_mmio_region {
    uint64_t base;
    uint64_t size;
    void (*access)(void *opaque, uint64_t offset, uint8_t *data, uint32_t len, int is_write);
    void *opaque;
};

/* 设备数很少，线性查找即可。 */
#define VCPU_MMIO_MAX 4u

struct vcpu {
    int fd;
    struct kvm_run *run;
    size_t run_size;
    /* vcpu_request_stop() 设置；可能在信号处理函数中写入。 */
    volatile sig_atomic_t stop_requested;
    pthread_t thread;
    volatile sig_atomic_t thread_valid;
    /*
     * 已注册的 MMIO 设备（Stage 06 virtio-blk、Stage 07 virtio-net）。
     * KVM_EXIT_MMIO 时按地址在表里查找；区间互不重叠，所以最多命中一个。
     */
    struct vcpu_mmio_region mmio[VCPU_MMIO_MAX];
    unsigned mmio_count;
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
    /* 分派给 MMIO 设备的 KVM_EXIT_MMIO 次数。 */
    unsigned long mmio_device_exits;
};

int vcpu_init(struct vcpu *vcpu, const struct kvm_context *kvm, unsigned id);
int vcpu_setup_linux_boot(struct vcpu *vcpu, unsigned kernel_addr,
                          unsigned boot_params_addr);
int vcpu_run(struct vcpu *vcpu, struct serial *serial,
             struct vcpu_run_stats *stats);
void vcpu_request_stop(struct vcpu *vcpu);
/*
 * 注册一个 MMIO 设备区间。size 为 0、区间回绕、与已注册区间重叠或表已满时
 * 打印错误并返回 -1（重叠意味着同一个地址有两个设备，是 VMM 配置错误）。
 * 必须在 vcpu_run() 之前调用。
 */
int vcpu_mmio_register(struct vcpu *vcpu, uint64_t base, uint64_t size,
                       void (*access)(void *opaque, uint64_t offset, uint8_t *data,
                                      uint32_t len, int is_write),
                       void *opaque);
void vcpu_destroy(struct vcpu *vcpu);

#endif

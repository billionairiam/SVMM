#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

/*
 * virtio-mmio 传输层（Virtio 1.0 §4.2，modern 即 Version 2）。
 *
 * 一个设备占客户机物理地址空间里的一页寄存器。这一页不注册成 KVM 内存槽，
 * 客户机每次读写都会 KVM_EXIT_MMIO 退出到 VMM，由 virtio_mmio_access() 模拟。
 * 寄存器只负责“控制面”：设备发现、特性协商、告诉设备 virtqueue 在哪、
 * doorbell（QueueNotify）和中断应答。真正的数据都在客户机内存里的 virtqueue 和
 * buffer 中，device 直接读写，不产生 VM exit。
 *
 * driver 初始化顺序（§3.1.1）：
 *   写 Status=0 复位 → ACKNOWLEDGE → DRIVER → 读 DeviceFeatures、写 DriverFeatures
 *   → FEATURES_OK（device 不接受时不保留这一位，driver 读回后放弃）
 *   → 对每个队列：QueueSel、读 QueueNumMax、写 QueueNum 和三块 ring 的 GPA、QueueReady=1
 *   → DRIVER_OK，之后才能 kick。
 *
 * 每个设备最多 VIRTIO_MMIO_MAX_QUEUES 个队列（virtio-blk 1 个，virtio-net 2 个），
 * 用 QueueSel 选择当前操作的队列。不支持 legacy（Version 1）接口、EVENT_IDX、
 * INDIRECT 和共享内存区。传输层与具体设备无关，设备通过 backend 回调接入。
 *
 * 并发：寄存器访问来自 vCPU 线程，Stage 07 的网卡还有一个 RX 线程会往队列里
 * 写帧、拉中断。所有设备状态都由 dev->lock 保护：virtio_mmio_access() 和
 * virtio_mmio_run_queue() 进出时加锁，backend 回调总是在持锁状态下被调用。
 */

#include "virtio/virtq.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 第一个设备的寄存器页地址和中断线（I/O APIC 的 GSI）。地址在最大客户机内存
 * （3 GiB）之上、IOAPIC/LAPIC 之下，Firecracker 的 x86 MMIO 设备区也从这里开始。
 * 内核从 DSDT 里的 \_SB.VR00（_HID "LNRO0005"）得知它的存在，见 boot/acpi.c。
 */
#define VIRTIO_MMIO_BASE 0xd0000000u
#define VIRTIO_MMIO_SIZE 0x1000u
#define VIRTIO_MMIO_IRQ 5u

/*
 * Stage 07：第二个设备（virtio-net）紧挨着第一个，用下一根中断线。
 * 每个设备独占一页寄存器和一根电平触发的 GSI，DSDT 里是 \_SB.VR01。
 */
#define VIRTIO_MMIO_NET_BASE (VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE)
#define VIRTIO_MMIO_NET_IRQ 6u

#define VIRTIO_MMIO_MAX_QUEUES 2u

/* 寄存器偏移（Virtio 1.0 §4.2.2）。除配置空间外都是 32 位、必须 4 字节对齐访问。 */
#define VIRTIO_MMIO_MAGIC_VALUE 0x000
#define VIRTIO_MMIO_VERSION 0x004
#define VIRTIO_MMIO_DEVICE_ID 0x008
#define VIRTIO_MMIO_VENDOR_ID 0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL 0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM 0x038
#define VIRTIO_MMIO_QUEUE_READY 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064
#define VIRTIO_MMIO_STATUS 0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090 /* 规范里叫 QueueDriver */
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW 0x0a0 /* 规范里叫 QueueDevice */
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4
#define VIRTIO_MMIO_SHM_SEL 0x0ac
#define VIRTIO_MMIO_SHM_LEN_LOW 0x0b0
#define VIRTIO_MMIO_SHM_LEN_HIGH 0x0b4
#define VIRTIO_MMIO_SHM_BASE_LOW 0x0b8
#define VIRTIO_MMIO_SHM_BASE_HIGH 0x0bc
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0fc
#define VIRTIO_MMIO_CONFIG 0x100 /* 设备专属配置空间，按任意宽度访问 */

#define VIRTIO_MMIO_MAGIC 0x74726976u /* 小端读出来是 "virt" */
#define VIRTIO_MMIO_VENDOR 0x4d4d5653u /* "SVMM" */

/* InterruptStatus 的两位。 */
#define VIRTIO_MMIO_INT_VRING 0x1u  /* used ring 有新完成项 */
#define VIRTIO_MMIO_INT_CONFIG 0x2u /* 配置空间或设备状态变化 */

/* Status 寄存器（§2.1）。 */
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define VIRTIO_STATUS_DRIVER 0x02u
#define VIRTIO_STATUS_DRIVER_OK 0x04u
#define VIRTIO_STATUS_FEATURES_OK 0x08u
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40u
#define VIRTIO_STATUS_FAILED 0x80u

/* 与设备类型无关的特性位。modern 设备必须提供 VERSION_1，Linux 也要求 driver 接受它。 */
#define VIRTIO_F_VERSION_1 32u

struct virtio_mmio;

struct virtio_mmio_backend {
    uint32_t device_id;
    uint64_t features; /* device 提供的特性位 */
    uint32_t num_queues; /* 1..VIRTIO_MMIO_MAX_QUEUES；编号 >= num_queues 的队列不存在 */
    uint16_t queue_num_max;
    /* 配置空间（从 0x100 起）的内容和大小；driver 只读。 */
    const void *config;
    uint32_t config_size;
    /*
     * 队列 queue 收到 kick 后调用（持有 dev->lock）：处理 avail ring 里的链，
     * 写好 used ring。设备也可以只记下“有新 buffer 了”，交给自己的线程处理
     * （virtio-net 的 RX 队列就是这样）。
     * 返回 0 表示正常；返回 -1 表示队列已无法继续（例如 avail.idx 非法），
     * 传输层随即置 DEVICE_NEEDS_RESET，直到 driver 复位设备。
     * 完成了几条链由传输层比较 vq->next_used_idx 得出，出错前完成的也会发中断。
     */
    int (*process_queue)(struct virtio_mmio *dev, uint32_t queue, struct virtq *vq);
    /*
     * 可选：driver 写入 DRIVER_OK、设备开始工作时调用（持有 dev->lock）。
     * 有自己线程的设备用它唤醒线程：DRIVER_OK 之前的 kick 会被忽略，
     * driver 可能已经放好了 buffer 却不会再 kick 一次。
     */
    void (*driver_ok)(struct virtio_mmio *dev);
};

/* 一个队列的配置；ready 之后 vq 指向客户机内存里的三块 ring。 */
struct virtio_mmio_queue {
    uint32_t num;
    uint64_t desc;
    uint64_t avail;
    uint64_t used;
    int ready;
    struct virtq vq;
};

struct virtio_mmio_stats {
    unsigned long reads;
    unsigned long writes;
    unsigned long notifies;      /* 被处理的 QueueNotify 写 */
    unsigned long interrupts;    /* 拉高中断线的次数 */
    unsigned long bad_accesses;  /* 宽度/对齐/偏移不合法的访问 */
};

struct virtio_mmio {
    pthread_mutex_t lock;
    const struct virtio_mmio_backend *backend;
    void *opaque; /* backend 私有数据 */

    /* 客户机 RAM 视图：GPA [0, mem_size) 对应 mem[0..mem_size)。 */
    uint8_t *mem;
    uint64_t mem_size;

    /* 中断线：level=1 拉高，0 拉低。可以为 NULL（只更新 InterruptStatus）。 */
    void (*set_irq)(void *opaque, int level);
    void *irq_opaque;

    /* driver 可见的寄存器状态。 */
    uint32_t status;
    uint32_t device_features_sel;
    uint32_t driver_features_sel;
    uint64_t driver_features;
    uint32_t queue_sel;
    uint32_t interrupt_status;
    int irq_level;

    /* 下标就是 QueueSel 的值。 */
    struct virtio_mmio_queue queues[VIRTIO_MMIO_MAX_QUEUES];

    struct virtio_mmio_stats stats;
};

/*
 * 初始化为复位状态。mem/mem_size 是客户机 RAM；set_irq 可以为 NULL，
 * 否则它可能在持有 dev->lock 的任意线程上被调用。
 */
void virtio_mmio_init(struct virtio_mmio *dev, const struct virtio_mmio_backend *backend,
                      void *opaque, uint8_t *mem, uint64_t mem_size,
                      void (*set_irq)(void *opaque, int level), void *irq_opaque);

/*
 * 模拟一次寄存器访问。offset 是相对寄存器页起点的偏移，len 是访问宽度
 * （1/2/4/8）。读时把结果写进 data（小端），写时从 data 读取。
 * 非法访问（越界、未对齐、宽度不对）不会失败：读返回 0，写被忽略，并计入
 * stats.bad_accesses。客户机怎么写这一页都不能让 VMM 出错。
 */
void virtio_mmio_access(struct virtio_mmio *dev, uint64_t offset, uint8_t *data,
                        uint32_t len, int is_write);

/*
 * 设备自己的线程（不是 kick）要处理队列时调用：持锁检查队列是否“活着”
 * （DRIVER_OK、队列 ready、没有 NEEDS_RESET/FAILED），是才调用 fn，然后和 kick
 * 一样根据 used ring 的变化拉中断；fn 返回 -1 时置 DEVICE_NEEDS_RESET。
 * 返回 1 表示 fn 被调用了，0 表示队列不可用（driver 还没准备好或正在复位）。
 */
int virtio_mmio_run_queue(struct virtio_mmio *dev, uint32_t queue,
                          int (*fn)(struct virtio_mmio *dev, struct virtq *vq, void *arg),
                          void *arg);

/*
 * 把 [gpa, gpa + len) 翻译成宿主指针。区间不完全落在客户机 RAM 里时返回 NULL，
 * 包括 gpa + len 溢出回绕。len 为 0 时只要求 gpa 不超过 RAM 末尾。
 */
void *virtio_mmio_gpa(const struct virtio_mmio *dev, uint64_t gpa, uint64_t len);

#endif

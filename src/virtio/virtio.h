#ifndef VIRTIO_VIRTIO_H
#define VIRTIO_VIRTIO_H

/*
 * 最小的 virtio 设备模型：一个设备 = 设备类型 + 一个 virtqueue + 客户机内存
 * 视图 + 三个回调。virtio_device_kick() 模拟 driver 写 doorbell 之后发生的事：
 *
 *   on_notify     收到通知（真实 VMM 中是 MMIO/PIO 写导致的 VM exit 或 ioeventfd）
 *   on_process    设备取走 avail 条目、遍历描述符链、做 I/O、写 used ring
 *   on_interrupt  有新完成项且 driver 没有关中断时，通知 driver（真实 VMM 中是 irqfd）
 *
 * 本阶段没有 transport（PCI/MMIO）和特性协商，这些留给 Stage 06。
 */

#include "virtio/virtq.h"

#include <stdint.h>

/* 设备 ID，取值见 Virtio 1.0 §5。 */
#define VIRTIO_ID_NET 1u
#define VIRTIO_ID_BLOCK 2u
#define VIRTIO_ID_CONSOLE 3u

struct virtio_device;

struct virtio_device_ops {
    void (*on_notify)(struct virtio_device *dev);
    /* 返回负数表示设备遇到无法继续的错误（规范中的 DEVICE_NEEDS_RESET）。 */
    int (*on_process)(struct virtio_device *dev);
    void (*on_interrupt)(struct virtio_device *dev);
};

struct virtio_device {
    uint32_t device_id;
    struct virtq *vq;
    const struct virtio_device_ops *ops;
    void *opaque; /* 设备私有数据 */
    /* 客户机物理内存：GPA 0 对应 mem[0]。描述符里的 addr 都按它翻译。 */
    uint8_t *mem;
    uint64_t mem_size;
    uint64_t notifications;
    uint64_t interrupts;
};

/* 未知 device_id、qsz 非法或 mem 为空时返回 NULL。 */
struct virtio_device *virtio_device_create(uint32_t device_id, unsigned int qsz,
                                           void *mem, uint64_t mem_size);
void virtio_device_destroy(struct virtio_device *dev);
void virtio_device_set_ops(struct virtio_device *dev, const struct virtio_device_ops *ops,
                           void *opaque);
const char *virtio_device_name(uint32_t device_id);

/*
 * 把 [gpa, gpa + len) 翻译成宿主指针。区间不完全落在客户机内存里时返回 NULL，
 * 包括 gpa + len 溢出回绕的情形。描述符内容由客户机控制，设备访问
 * buffer 前必须经过这一步。
 */
void *virtio_gpa(const struct virtio_device *dev, uint64_t gpa, uint64_t len);

/*
 * driver 发布 avail 条目后调用：on_notify → on_process → 视情况 on_interrupt。
 * 返回本次新完成（used.idx 前进）的条目数；on_process 失败时返回 -1，
 * 但已完成的条目仍会触发中断。
 */
int virtio_device_kick(struct virtio_device *dev);

#endif

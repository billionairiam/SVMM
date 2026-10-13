#ifndef VIRTIO_VIRTQ_H
#define VIRTIO_VIRTQ_H

/*
 * Split virtqueue（Virtio 1.0 §2.6），纯用户态实现，不依赖 KVM。
 *
 * 一个队列由三块共享内存组成：
 *   descriptor table  每项描述一段 buffer（GPA + 长度 + 标志），可用 NEXT 串成链；
 *   available ring    driver 写、device 读：“这些链等你处理”；
 *   used ring         device 写、driver 读：“这些链处理完了，写入了多少字节”。
 *
 * 本阶段三块内存由 virtq_create() 在宿主进程里分配；描述符里的 addr 是
 * 客户机物理地址（GPA），由上层（virtio.h 的 virtio_gpa()）翻译。Stage 06
 * 会把它们放进真正的客户机内存。
 *
 * 规范要求这些字段是小端；本项目只支持小端宿主（x86_64、arm64），不做转换。
 */

#include <stdint.h>
#include <stdio.h>

/* 规范允许最大 32768；教学实现限制在 256，便于 dump 和遍历时的 visited 表。 */
#define VIRTQ_MAX_SIZE 256u

#define VRING_DESC_F_NEXT 0x1u     /* 链未结束，next 有效 */
#define VRING_DESC_F_WRITE 0x2u    /* buffer 对 device 可写（否则只读） */
#define VRING_DESC_F_INDIRECT 0x4u /* addr 指向间接描述符表；本阶段不支持 */

#define VRING_AVAIL_F_NO_INTERRUPT 0x1u /* driver：完成后不必发中断 */
#define VRING_USED_F_NO_NOTIFY 0x1u     /* device：提交后不必 kick */

struct vring_desc {
    uint64_t addr;  /* buffer 的客户机物理地址 */
    uint32_t len;   /* buffer 字节数 */
    uint16_t flags; /* VRING_DESC_F_* */
    uint16_t next;  /* flags 含 NEXT 时，链中下一项的下标 */
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;   /* driver 发布过的链总数，16 位回绕 */
    uint16_t ring[]; /* [qsz] 链头下标；之后还有 used_event（EVENT_IDX 才用） */
};

struct vring_used_elem {
    uint32_t id;  /* 完成的链头下标 */
    uint32_t len; /* device 写进可写 buffer 的总字节数 */
};

struct vring_used {
    uint16_t flags;
    uint16_t idx; /* device 完成过的链总数，16 位回绕 */
    struct vring_used_elem ring[]; /* [qsz]；之后还有 avail_event */
};

struct virtq {
    uint16_t qsz; /* 2 的幂；ring 槽位 = 计数器 & (qsz - 1) */
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;

    /*
     * 以下是两端各自的私有状态，不在共享内存里。真实系统中 driver 和 device
     * 互相看不到对方的私有状态，所以每个“满/空/非法”的判断都只能用本端的
     * 计数器加上共享 ring 里的 idx。
     */
    uint16_t next_avail_idx; /* driver：下一次发布写到 avail.idx 的值 */
    uint16_t last_used_idx;  /* driver：已回收到的 used 计数 */
    uint16_t last_seen_avail; /* device：已取走的 avail 计数 */
    uint16_t next_used_idx;   /* device：下一次完成写到 used.idx 的值 */
};

/* qsz 必须是 [2, VIRTQ_MAX_SIZE] 内的 2 的幂，否则返回 NULL。 */
struct virtq *virtq_create(unsigned int qsz);
void virtq_destroy(struct virtq *vq);

/* ---- driver 侧（客户机做的事） ---- */

/* 填写 desc[idx]。idx 或 next 越界、flags 含未知位时返回 -1。 */
int virtq_desc_fill(struct virtq *vq, uint16_t idx, uint64_t addr, uint32_t len,
                    uint16_t flags, uint16_t next);
/*
 * 把链头 head 写进 avail ring，然后发布新的 avail.idx。
 * 在途（已发布、还没从 used ring 回收）的链已有 qsz 条时返回 -1：再写就会
 * 覆盖 device 可能还没读的槽位。
 */
int virtq_avail_submit(struct virtq *vq, uint16_t head);
/*
 * 回收一条完成项。返回 1 表示取到，0 表示没有新完成项；
 * device 发布的 used.idx 超过在途数量或 id 越界时返回 -1（device 有 bug 或恶意）。
 */
int virtq_used_pop(struct virtq *vq, struct vring_used_elem *elem);
/* 返回 1 表示需要 kick device；device 设置了 VRING_USED_F_NO_NOTIFY 时返回 0。 */
int virtq_needs_notify(const struct virtq *vq);

/* ---- device 侧（VMM 做的事） ---- */

/*
 * 查看（peek）或取走（consume）下一条待处理链的链头。返回 1 表示有，0 表示空；
 * driver 发布的 avail.idx 一次前进超过 qsz 时返回 -1（driver 有 bug 或恶意）。
 * peek 不改变任何状态。
 */
int virtq_avail_peek(const struct virtq *vq, uint16_t *head);
int virtq_avail_consume(struct virtq *vq, uint16_t *head);

typedef int (*virtq_desc_fn)(const struct vring_desc *desc, uint16_t idx, void *opaque);
/*
 * 从 head 开始沿 NEXT 遍历一条链，对每个描述符调用 fn（fn 可为 NULL）。
 * 先完整校验整条链，再调用 fn，所以非法链不会让设备做一半 I/O。
 * 非法情形：下标越界、成环、INDIRECT、可写描述符之后又出现只读描述符。
 * 返回链中描述符个数；链非法或 fn 返回负数时返回 -1。
 */
int virtq_walk_chain(const struct virtq *vq, uint16_t head, virtq_desc_fn fn, void *opaque);
/*
 * 把 {head, len} 写进 used ring 并发布新的 used.idx。
 * head 越界，或当前没有“已取走但未完成”的链（无从完成，也防止 used ring 溢出）
 * 时返回 -1。
 */
int virtq_used_push(struct virtq *vq, uint16_t head, uint32_t len);
/* driver 是否请求过“不必发中断”（VRING_AVAIL_F_NO_INTERRUPT）。 */
int virtq_needs_interrupt(const struct virtq *vq);

/* ---- 统计与调试 ---- */

/* device 视角：avail ring 中尚未取走的条目数（avail.idx - last_seen_avail）。 */
unsigned int virtq_avail_count(const struct virtq *vq);
/* driver 视角：used ring 中尚未回收的条目数（used.idx - last_used_idx）。 */
unsigned int virtq_used_count(const struct virtq *vq);
/* 打印描述符表、avail ring（device 视角）和 used ring（driver 视角）。 */
void virtq_dump(const struct virtq *vq, FILE *out);
/* 打印一条链；链非法时打印原因并返回 -1。 */
int virtq_dump_chain(const struct virtq *vq, uint16_t head, FILE *out);

#endif

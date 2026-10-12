/*
 * Stage 05：split virtqueue 和设备模型的失败方式测试。
 *
 * virtqueue 是 guest 与 VMM 之间的共享内存协议，两端都不能信任对方写进
 * 共享内存的内容。先列出它可能失败的方式，下面每个测试对应其中一条：
 *
 *   1. 非法队列大小（0、1、非 2 的幂、超过上限）被接受，之后的槽位掩码
 *      运算 (idx & (qsz-1)) 静默错位。
 *   2. 16 位计数器回绕（65535 → 0）时把差值算错：请求被丢掉、重复处理，
 *      或 ring 被误判为满/空。
 *   3. driver 在途请求超过 qsz 时继续发布，覆盖 device 还没读的 avail 槽位。
 *   4. device 完成一条从没取走过的链（伪造完成），或把 used ring 写溢出。
 *   5. peek 改变了状态；consume 打乱 FIFO 顺序。
 *   6. 恶意描述符链让 device 越界读、死循环或做一半 I/O：next 越界、
 *      自环、0→1→0 环、INDIRECT、可写描述符之后又是只读描述符、
 *      avail ring 里的链头本身越界。
 *   7. 恶意 driver 让 avail.idx 一次前进超过 qsz，device 把旧槽位当新请求。
 *   8. 恶意/有 bug 的 device 让 used.idx 超过在途数量或写入越界 id，
 *      driver 回收不存在的请求。
 *   9. 描述符 addr/len 指向客户机内存之外，或 addr + len 溢出回绕，
 *      device 读写到宿主内存。
 *  10. 通知语义错误：没有完成项也发中断；有完成项却漏发；
 *      无视 VRING_AVAIL_F_NO_INTERRUPT；on_process 出错时吞掉已完成项的中断。
 *  11. desc_fill 接受越界下标或未知 flag。
 */
#include "virtio/virtio.h"
#include "virtio/virtq.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, \
                    __func__, #cond);                                            \
            failures++;                                                          \
        }                                                                        \
    } while (0)

/* 下面若干测试需要一个“device 取走 → 遍历 → 完成”的闭环。 */
static int complete_next(struct virtq *vq, uint32_t len, uint16_t *head)
{
    if (virtq_avail_consume(vq, head) != 1)
        return -1;
    if (virtq_walk_chain(vq, *head, NULL, NULL) < 0)
        return -1;
    return virtq_used_push(vq, *head, len);
}

static int count_calls(const struct vring_desc *desc, uint16_t idx, void *opaque)
{
    (void)desc;
    (void)idx;
    ++*(int *)opaque;
    return 0;
}

/* 1 */
static void test_invalid_queue_sizes_are_rejected(void)
{
    const unsigned int bad[] = { 0, 1, 3, 6, 100, 257, 512, 65536, 65538 };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct virtq *vq = virtq_create(bad[i]);
        CHECK(vq == NULL);
        virtq_destroy(vq);
    }
    for (unsigned int qsz = 2; qsz <= VIRTQ_MAX_SIZE; qsz *= 2) {
        struct virtq *vq = virtq_create(qsz);
        CHECK(vq && vq->qsz == qsz && vq->desc && vq->avail && vq->used);
        CHECK(vq && virtq_avail_count(vq) == 0 && virtq_used_count(vq) == 0);
        virtq_destroy(vq);
    }
    virtq_destroy(NULL);
}

/* 2：批量在途请求跨过 65535 → 0，每条都必须按顺序原样回来。 */
static void test_counters_survive_16bit_wraparound(void)
{
    struct virtq *vq = virtq_create(4);
    for (uint16_t i = 0; i < 4; i++)
        CHECK(virtq_desc_fill(vq, i, 0x1000u * i, 16, 0, 0) == 0);

    unsigned long rounds = 0;
    int wrapped = 0;
    for (unsigned long batch = 0; batch < 30000; batch++) {
        /* 每批 3 条，所以 idx 在回绕时有请求跨在 65535 和 0 两侧。 */
        for (uint16_t i = 0; i < 3; i++) {
            uint16_t head = (uint16_t)((batch + i) & 3);
            CHECK(virtq_avail_submit(vq, head) == 0);
        }
        CHECK(virtq_avail_count(vq) == 3);
        for (uint16_t i = 0; i < 3; i++) {
            uint16_t head;
            CHECK(complete_next(vq, 100 + i, &head) == 0);
            CHECK(head == ((batch + i) & 3));
        }
        CHECK(virtq_avail_count(vq) == 0);
        CHECK(virtq_used_count(vq) == 3);
        for (uint16_t i = 0; i < 3; i++) {
            struct vring_used_elem elem;
            CHECK(virtq_used_pop(vq, &elem) == 1);
            CHECK(elem.id == ((batch + i) & 3) && elem.len == 100u + i);
            rounds++;
        }
        if (vq->avail->idx < 3)
            wrapped = 1;
        if (failures)
            break;
    }
    CHECK(wrapped);
    CHECK(vq->avail->idx == (uint16_t)rounds && vq->used->idx == (uint16_t)rounds);
    struct vring_used_elem elem;
    CHECK(virtq_used_pop(vq, &elem) == 0);
    virtq_destroy(vq);
}

/* 3 */
static void test_driver_cannot_overrun_inflight_limit(void)
{
    struct virtq *vq = virtq_create(4);
    for (uint16_t i = 0; i < 4; i++) {
        CHECK(virtq_desc_fill(vq, i, 0x1000, 8, 0, 0) == 0);
        CHECK(virtq_avail_submit(vq, i) == 0);
    }
    uint16_t idx_before = vq->avail->idx;
    uint16_t slot0 = vq->avail->ring[0];
    CHECK(virtq_avail_submit(vq, 0) == -1);
    CHECK(vq->avail->idx == idx_before && vq->avail->ring[0] == slot0);

    /* device 取走一条但还没完成：在途仍是 4，不能发布。 */
    uint16_t head;
    CHECK(virtq_avail_consume(vq, &head) == 1 && head == 0);
    CHECK(virtq_avail_submit(vq, 0) == -1);
    /* 完成但 driver 还没回收：仍不能发布（driver 无法知道 used 槽位已被读）。 */
    CHECK(virtq_used_push(vq, head, 0) == 0);
    CHECK(virtq_avail_submit(vq, 0) == -1);
    struct vring_used_elem elem;
    CHECK(virtq_used_pop(vq, &elem) == 1 && elem.id == 0);
    CHECK(virtq_avail_submit(vq, 0) == 0);
    CHECK(virtq_avail_submit(vq, 1) == -1);

    CHECK(virtq_avail_submit(vq, 4) == -1); /* 链头越界 */
    virtq_destroy(vq);
}

/* 4 */
static void test_device_cannot_complete_unconsumed_chains(void)
{
    struct virtq *vq = virtq_create(2);
    CHECK(virtq_used_push(vq, 0, 0) == -1); /* 什么都没取走 */
    CHECK(vq->used->idx == 0);

    CHECK(virtq_desc_fill(vq, 0, 0x1000, 8, 0, 0) == 0);
    CHECK(virtq_desc_fill(vq, 1, 0x2000, 8, 0, 0) == 0);
    CHECK(virtq_avail_submit(vq, 0) == 0);
    CHECK(virtq_avail_submit(vq, 1) == 0);
    CHECK(virtq_used_push(vq, 0, 0) == -1); /* 已发布但还没取走 */

    uint16_t head;
    CHECK(virtq_avail_consume(vq, &head) == 1);
    CHECK(virtq_used_push(vq, 2, 0) == -1); /* id 越界 */
    CHECK(virtq_used_push(vq, head, 0) == 0);
    CHECK(virtq_used_push(vq, head, 0) == -1); /* 同一次 consume 不能完成两次 */
    CHECK(vq->used->idx == 1);
    virtq_destroy(vq);
}

/* 5 */
static void test_peek_is_pure_and_consume_is_fifo(void)
{
    struct virtq *vq = virtq_create(8);
    uint16_t head = 0xffff;
    CHECK(virtq_avail_peek(vq, &head) == 0 && head == 0xffff);
    CHECK(virtq_avail_consume(vq, &head) == 0);

    const uint16_t order[] = { 5, 2, 7 };
    for (int i = 0; i < 3; i++) {
        CHECK(virtq_desc_fill(vq, order[i], 0x1000, 8, 0, 0) == 0);
        CHECK(virtq_avail_submit(vq, order[i]) == 0);
    }
    for (int i = 0; i < 3; i++) {
        CHECK(virtq_avail_peek(vq, &head) == 1 && head == order[i]);
        CHECK(virtq_avail_peek(vq, &head) == 1 && head == order[i]);
        CHECK(virtq_avail_count(vq) == 3u - i);
        CHECK(virtq_avail_consume(vq, &head) == 1 && head == order[i]);
    }
    CHECK(virtq_avail_peek(vq, &head) == 0);
    virtq_destroy(vq);
}

/* 6：直接写共享内存模拟恶意 driver，绕过 virtq_desc_fill 的检查。 */
static void raw_desc(struct virtq *vq, uint16_t idx, uint32_t len, uint16_t flags, uint16_t next)
{
    vq->desc[idx] = (struct vring_desc){ .addr = 0x1000u * idx, .len = len,
                                         .flags = flags, .next = next };
}

static void test_malicious_chains_are_rejected_without_side_effects(void)
{
    struct virtq *vq = virtq_create(4);
    int calls = 0;

    /* 合法基线：RO → WO → WO，回调按链顺序被调用 3 次。 */
    raw_desc(vq, 0, 16, VRING_DESC_F_NEXT, 2);
    raw_desc(vq, 2, 512, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 1);
    raw_desc(vq, 1, 1, VRING_DESC_F_WRITE, 0);
    CHECK(virtq_walk_chain(vq, 0, count_calls, &calls) == 3 && calls == 3);

    calls = 0; /* 链头越界 */
    CHECK(virtq_walk_chain(vq, 4, count_calls, &calls) == -1 && calls == 0);

    raw_desc(vq, 0, 16, VRING_DESC_F_NEXT, 9); /* next 越界 */
    calls = 0;
    CHECK(virtq_walk_chain(vq, 0, count_calls, &calls) == -1 && calls == 0);

    raw_desc(vq, 3, 16, VRING_DESC_F_NEXT, 3); /* 自环 */
    calls = 0;
    CHECK(virtq_walk_chain(vq, 3, count_calls, &calls) == -1 && calls == 0);

    raw_desc(vq, 0, 16, VRING_DESC_F_NEXT, 1); /* 0 → 1 → 0 */
    raw_desc(vq, 1, 16, VRING_DESC_F_NEXT, 0);
    calls = 0;
    CHECK(virtq_walk_chain(vq, 0, count_calls, &calls) == -1 && calls == 0);

    raw_desc(vq, 0, 16, VRING_DESC_F_NEXT, 1); /* 0 → 1 → 2 → 3 → 1：环不经过链头 */
    raw_desc(vq, 1, 16, VRING_DESC_F_NEXT, 2);
    raw_desc(vq, 2, 16, VRING_DESC_F_NEXT, 3);
    raw_desc(vq, 3, 16, VRING_DESC_F_NEXT, 1);
    calls = 0;
    CHECK(virtq_walk_chain(vq, 0, count_calls, &calls) == -1 && calls == 0);

    raw_desc(vq, 0, 16, VRING_DESC_F_NEXT, 1); /* INDIRECT 出现在链尾 */
    raw_desc(vq, 1, 64, VRING_DESC_F_INDIRECT, 0);
    calls = 0;
    CHECK(virtq_walk_chain(vq, 0, count_calls, &calls) == -1 && calls == 0);

    raw_desc(vq, 0, 512, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 1); /* WO 后跟 RO */
    raw_desc(vq, 1, 16, 0, 0);
    calls = 0;
    CHECK(virtq_walk_chain(vq, 0, count_calls, &calls) == -1 && calls == 0);

    /* 链长恰好 qsz（每个描述符各用一次）是合法的。 */
    for (uint16_t i = 0; i < 4; i++)
        raw_desc(vq, i, 8, i == 3 ? 0 : VRING_DESC_F_NEXT, (uint16_t)(i + 1));
    calls = 0;
    CHECK(virtq_walk_chain(vq, 0, count_calls, &calls) == 4 && calls == 4);

    /* 恶意 driver 直接把越界链头写进 avail ring：consume 取得到，walk 必须拒绝。 */
    vq->avail->ring[0] = 1000;
    vq->avail->idx = 1;
    uint16_t head;
    CHECK(virtq_avail_consume(vq, &head) == 1 && head == 1000);
    calls = 0;
    CHECK(virtq_walk_chain(vq, head, count_calls, &calls) == -1 && calls == 0);
    CHECK(virtq_used_push(vq, head, 0) == -1);
    CHECK(virtq_dump_chain(vq, head, stdout) == -1);
    virtq_destroy(vq);
}

/* 7 */
static void test_device_rejects_avail_idx_jump(void)
{
    struct virtq *vq = virtq_create(4);
    vq->avail->idx = 5; /* last_seen = 0，一次前进 5 > qsz */
    uint16_t head;
    CHECK(virtq_avail_peek(vq, &head) == -1);
    CHECK(virtq_avail_consume(vq, &head) == -1);
    CHECK(vq->last_seen_avail == 0);

    vq->avail->idx = 4; /* 恰好 qsz 是合法的 */
    CHECK(virtq_avail_consume(vq, &head) == 1);

    /* 回绕方向的跳跃同样非法：0 - 1 = 65535 条。 */
    struct virtq *vq2 = virtq_create(4);
    vq2->avail->idx = 0xffff;
    CHECK(virtq_avail_consume(vq2, &head) == -1);
    virtq_destroy(vq2);
    virtq_destroy(vq);
}

/* 8 */
static void test_driver_rejects_bogus_used_entries(void)
{
    struct virtq *vq = virtq_create(4);
    struct vring_used_elem elem;

    vq->used->idx = 1; /* 没有任何在途请求，device 却声称完成了一条 */
    vq->used->ring[0] = (struct vring_used_elem){ .id = 0, .len = 0 };
    CHECK(virtq_used_pop(vq, &elem) == -1);
    CHECK(vq->last_used_idx == 0);

    vq->used->idx = 0;
    CHECK(virtq_desc_fill(vq, 0, 0x1000, 8, 0, 0) == 0);
    CHECK(virtq_avail_submit(vq, 0) == 0);
    vq->used->ring[0] = (struct vring_used_elem){ .id = 99, .len = 0 }; /* id 越界 */
    vq->used->idx = 1;
    CHECK(virtq_used_pop(vq, &elem) == -1);
    CHECK(vq->last_used_idx == 0);

    vq->used->ring[0].id = 0;
    CHECK(virtq_used_pop(vq, &elem) == 1 && elem.id == 0);
    virtq_destroy(vq);
}

/* 9 */
static void test_gpa_translation_is_bounds_checked(void)
{
    static uint8_t ram[4096];
    struct virtio_device *dev = virtio_device_create(VIRTIO_ID_BLOCK, 4, ram, sizeof(ram));
    CHECK(dev != NULL);
    if (!dev)
        return;
    CHECK(virtio_gpa(dev, 0, sizeof(ram)) == ram);
    CHECK(virtio_gpa(dev, 4095, 1) == ram + 4095);
    CHECK(virtio_gpa(dev, 4096, 0) == ram + 4096); /* 空区间落在末尾是合法的 */
    CHECK(virtio_gpa(dev, 4097, 0) == NULL);
    CHECK(virtio_gpa(dev, 4095, 2) == NULL);
    CHECK(virtio_gpa(dev, 4096, 1) == NULL);
    CHECK(virtio_gpa(dev, UINT64_MAX, 2) == NULL);  /* gpa + len 回绕到 1 */
    CHECK(virtio_gpa(dev, 16, UINT64_MAX) == NULL);
    virtio_device_destroy(dev);

    CHECK(virtio_device_create(0, 4, ram, sizeof(ram)) == NULL);
    CHECK(virtio_device_create(42, 4, ram, sizeof(ram)) == NULL);
    CHECK(virtio_device_create(VIRTIO_ID_NET, 3, ram, sizeof(ram)) == NULL);
    CHECK(virtio_device_create(VIRTIO_ID_NET, 4, NULL, sizeof(ram)) == NULL);
}

/* 10 */
struct kick_log {
    int notify, process, interrupt;
    int fail;
};

static int process_all(struct virtio_device *dev)
{
    struct kick_log *log = dev->opaque;
    log->process++;
    uint16_t head;
    int rc;
    while ((rc = virtq_avail_consume(dev->vq, &head)) == 1) {
        if (virtq_walk_chain(dev->vq, head, NULL, NULL) < 0)
            return -1;
        if (virtq_used_push(dev->vq, head, 0) < 0)
            return -1;
        if (log->fail)
            return -1; /* 完成一条之后才出错 */
    }
    return rc;
}

static void on_notify(struct virtio_device *dev) { ((struct kick_log *)dev->opaque)->notify++; }
static void on_interrupt(struct virtio_device *dev) { ((struct kick_log *)dev->opaque)->interrupt++; }

static void test_kick_interrupt_semantics(void)
{
    static uint8_t ram[4096];
    static const struct virtio_device_ops ops = {
        .on_notify = on_notify, .on_process = process_all, .on_interrupt = on_interrupt,
    };
    struct kick_log log = { 0 };
    struct virtio_device *dev = virtio_device_create(VIRTIO_ID_CONSOLE, 4, ram, sizeof(ram));
    CHECK(dev != NULL);
    if (!dev)
        return;
    virtio_device_set_ops(dev, &ops, &log);
    struct virtq *vq = dev->vq;
    struct vring_used_elem elem;

    /* 空 kick：通知和处理都发生，但不能有中断。 */
    CHECK(virtio_device_kick(dev) == 0);
    CHECK(log.notify == 1 && log.process == 1 && log.interrupt == 0);

    /* 一次 kick 处理两条请求：中断只发一次（批处理合并中断）。 */
    CHECK(virtq_desc_fill(vq, 0, 0, 8, 0, 0) == 0);
    CHECK(virtq_desc_fill(vq, 1, 8, 8, 0, 0) == 0);
    CHECK(virtq_avail_submit(vq, 0) == 0 && virtq_avail_submit(vq, 1) == 0);
    CHECK(virtio_device_kick(dev) == 2);
    CHECK(log.interrupt == 1 && dev->interrupts == 1 && dev->notifications == 2);
    CHECK(virtq_used_pop(vq, &elem) == 1 && virtq_used_pop(vq, &elem) == 1);

    /* driver 关中断（轮询模式）：有完成项也不发。 */
    vq->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    CHECK(virtq_avail_submit(vq, 0) == 0);
    CHECK(virtio_device_kick(dev) == 1);
    CHECK(log.interrupt == 1);
    CHECK(virtq_used_pop(vq, &elem) == 1);
    vq->avail->flags = 0;

    /* on_process 在完成一条后出错：返回 -1，但已完成的那条仍要通知 driver。 */
    log.fail = 1;
    CHECK(virtq_avail_submit(vq, 0) == 0);
    CHECK(virtio_device_kick(dev) == -1);
    CHECK(log.interrupt == 2);
    CHECK(virtq_used_pop(vq, &elem) == 1);

    /* 未设置回调的设备 kick 不崩溃。 */
    virtio_device_set_ops(dev, NULL, NULL);
    CHECK(virtio_device_kick(dev) == 0);
    virtio_device_destroy(dev);
}

/* 11 */
static void test_desc_fill_validates_input(void)
{
    struct virtq *vq = virtq_create(4);
    CHECK(virtq_desc_fill(vq, 4, 0, 8, 0, 0) == -1);
    CHECK(virtq_desc_fill(vq, 0, 0, 8, VRING_DESC_F_NEXT, 4) == -1);
    CHECK(virtq_desc_fill(vq, 0, 0, 8, 0x8, 0) == -1);
    CHECK(virtq_desc_fill(vq, 0, 0, 8, VRING_DESC_F_INDIRECT, 0) == -1);
    CHECK(vq->desc[0].len == 0);
    CHECK(virtq_desc_fill(vq, 3, 0x2000, 512, VRING_DESC_F_WRITE, 0) == 0);
    CHECK(vq->desc[3].addr == 0x2000 && vq->desc[3].len == 512 &&
          vq->desc[3].flags == VRING_DESC_F_WRITE);
    virtq_destroy(vq);
}

int main(void)
{
    test_invalid_queue_sizes_are_rejected();
    test_counters_survive_16bit_wraparound();
    test_driver_cannot_overrun_inflight_limit();
    test_device_cannot_complete_unconsumed_chains();
    test_peek_is_pure_and_consume_is_fifo();
    test_malicious_chains_are_rejected_without_side_effects();
    test_device_rejects_avail_idx_jump();
    test_driver_rejects_bogus_used_entries();
    test_gpa_translation_is_bounds_checked();
    test_kick_interrupt_semantics();
    test_desc_fill_validates_input();
    if (failures) {
        fprintf(stderr, "test_virtq: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_virtq: all checks passed\n");
    return 0;
}

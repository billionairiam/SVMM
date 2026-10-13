#include "virtio/virtq.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

/*
 * 共享 ring 的 idx 是发布点：写者先写好描述符/ring 槽位，再以 release 语义
 * 写 idx；读者以 acquire 语义读 idx，之后读到的槽位内容一定是新的。
 * 本阶段是单线程，屏障不改变结果，但顺序与真实的多线程 VMM 一致。
 */
static uint16_t load_idx(const uint16_t *idx)
{
    return __atomic_load_n(idx, __ATOMIC_ACQUIRE);
}

static void publish_idx(uint16_t *idx, uint16_t value)
{
    __atomic_store_n(idx, value, __ATOMIC_RELEASE);
}

static uint16_t slot(const struct virtq *vq, uint16_t counter)
{
    return counter & (uint16_t)(vq->qsz - 1);
}

static bool valid_qsz(unsigned int qsz)
{
    return qsz >= 2 && qsz <= VIRTQ_MAX_SIZE && !(qsz & (qsz - 1));
}

uint64_t virtq_desc_bytes(unsigned int qsz)
{
    return (uint64_t)sizeof(struct vring_desc) * qsz;
}

/* flags + idx + ring[qsz] + used_event */
uint64_t virtq_avail_bytes(unsigned int qsz)
{
    return sizeof(struct vring_avail) + (uint64_t)sizeof(uint16_t) * (qsz + 1);
}

/* flags + idx + ring[qsz] + avail_event */
uint64_t virtq_used_bytes(unsigned int qsz)
{
    return sizeof(struct vring_used) + (uint64_t)sizeof(struct vring_used_elem) * qsz +
           sizeof(uint16_t);
}

int virtq_init_shared(struct virtq *vq, unsigned int qsz, struct vring_desc *desc,
                      struct vring_avail *avail, struct vring_used *used)
{
    if (!valid_qsz(qsz) || !desc || !avail || !used)
        return -1;
    /*
     * 复位后 driver 把 ring 清零再交给 device（§2.6），两端计数器都从 0 开始。
     * 这里不写共享内存：ring 内容归 driver 初始化。
     */
    *vq = (struct virtq){ .qsz = (uint16_t)qsz, .desc = desc, .avail = avail, .used = used };
    return 0;
}

struct virtq *virtq_create(unsigned int qsz)
{
    if (!valid_qsz(qsz))
        return NULL;
    struct virtq *vq = calloc(1, sizeof(*vq));
    if (!vq)
        return NULL;
    vq->qsz = (uint16_t)qsz;
    /*
     * 规范 §2.6 的对齐要求：desc 16 字节、avail 2 字节、used 4 字节，
     * calloc 都满足。ring 之后的 used_event / avail_event 只在协商了
     * VIRTIO_F_EVENT_IDX 时使用，这里按规范大小预留但不使用。
     */
    vq->desc = calloc(1, virtq_desc_bytes(qsz));
    vq->avail = calloc(1, virtq_avail_bytes(qsz));
    vq->used = calloc(1, virtq_used_bytes(qsz));
    if (!vq->desc || !vq->avail || !vq->used) {
        virtq_destroy(vq);
        return NULL;
    }
    return vq;
}

void virtq_destroy(struct virtq *vq)
{
    if (!vq)
        return;
    free(vq->desc);
    free(vq->avail);
    free(vq->used);
    free(vq);
}

/* ---- driver 侧 ---- */

int virtq_desc_fill(struct virtq *vq, uint16_t idx, uint64_t addr, uint32_t len,
                    uint16_t flags, uint16_t next)
{
    if (idx >= vq->qsz || (flags & ~(VRING_DESC_F_NEXT | VRING_DESC_F_WRITE)))
        return -1;
    if ((flags & VRING_DESC_F_NEXT) && next >= vq->qsz)
        return -1;
    vq->desc[idx] = (struct vring_desc){ .addr = addr, .len = len, .flags = flags, .next = next };
    return 0;
}

/* driver 已发布但还没回收的链数。每条链至少占一个描述符，所以不会超过 qsz。 */
static uint16_t driver_inflight(const struct virtq *vq)
{
    return (uint16_t)(vq->next_avail_idx - vq->last_used_idx);
}

int virtq_avail_submit(struct virtq *vq, uint16_t head)
{
    if (head >= vq->qsz || driver_inflight(vq) >= vq->qsz)
        return -1;
    vq->avail->ring[slot(vq, vq->next_avail_idx)] = head;
    vq->next_avail_idx++;
    publish_idx(&vq->avail->idx, vq->next_avail_idx);
    return 0;
}

int virtq_used_pop(struct virtq *vq, struct vring_used_elem *elem)
{
    uint16_t pending = (uint16_t)(load_idx(&vq->used->idx) - vq->last_used_idx);
    if (pending == 0)
        return 0;
    if (pending > driver_inflight(vq))
        return -1;
    struct vring_used_elem got = vq->used->ring[slot(vq, vq->last_used_idx)];
    if (got.id >= vq->qsz)
        return -1;
    vq->last_used_idx++;
    *elem = got;
    return 1;
}

/*
 * driver 提交 avail 条目后，用它判断要不要 kick（写 doorbell）device。
 * 返回 1 表示需要 kick，0 表示 device 说过“不必 kick”。
 *
 * 和 virtq_needs_interrupt() 是一对，方向相反：
 *   used->flags  的 NO_NOTIFY     由 device 设置，告诉 driver 不必 kick；
 *   avail->flags 的 NO_INTERRUPT  由 driver 设置，告诉 device 不必发中断。
 * 每个标志都放在“对方只读、自己能写”的那个 ring 上。
 *
 * device 正在主动处理 avail ring 时（比如 vhost 线程忙着干活）会设置 NO_NOTIFY：
 * 新条目它这一轮自己就能看到，而真实 VMM 中一次 kick 是一次 MMIO/PIO 写，
 * 会引发 VM exit，开销很大。规范规定这只是提示，多 kick 一次不会出错，
 * 只是浪费；但该 kick 的时候没 kick，请求就会一直没人处理。
 *
 * flags 由另一侧写，所以用原子读，防止编译器把它缓存在寄存器里，也避免读到写了一半的值。
 * 注意：真正多线程时，driver “写 avail.idx 再读 used->flags” 是先写后读，
 * release/acquire 不保证这两步的顺序，需要一个完整内存屏障（Linux 用 virtio_mb()）；
 * 目前 driver 和 device 在同一线程，不受影响。
 */
int virtq_needs_notify(const struct virtq *vq)
{
    return !(__atomic_load_n(&vq->used->flags, __ATOMIC_ACQUIRE) & VRING_USED_F_NO_NOTIFY);
}

/* ---- device 侧 ---- */

int virtq_avail_peek(const struct virtq *vq, uint16_t *head)
{
    uint16_t pending = (uint16_t)(load_idx(&vq->avail->idx) - vq->last_seen_avail);
    if (pending == 0)
        return 0;
    if (pending > vq->qsz)
        return -1;
    *head = vq->avail->ring[slot(vq, vq->last_seen_avail)];
    return 1;
}

int virtq_avail_consume(struct virtq *vq, uint16_t *head)
{
    int rc = virtq_avail_peek(vq, head);
    if (rc == 1)
        vq->last_seen_avail++;
    return rc;
}

/*
 * 校验整条链，并把描述符拷贝到 chain[]。之后只使用这份快照：客户机可以
 * 在校验之后改写共享的描述符表（double fetch），再读一次就可能绕过校验。
 */
static int collect_chain(const struct virtq *vq, uint16_t head,
                         struct vring_desc chain[VIRTQ_MAX_SIZE],
                         uint16_t index[VIRTQ_MAX_SIZE], const char **reason)
{
    bool visited[VIRTQ_MAX_SIZE] = { false };
    bool seen_writable = false;
    uint16_t idx = head;
    int count = 0;

    for (;;) {
        if (idx >= vq->qsz) {
            *reason = count ? "next index out of range" : "head index out of range";
            return -1;
        }
        if (visited[idx]) {
            *reason = "descriptor loop";
            return -1;
        }
        visited[idx] = true;
        struct vring_desc desc = vq->desc[idx];
        if (desc.flags & VRING_DESC_F_INDIRECT) {
            *reason = "indirect descriptors are not supported";
            return -1;
        }
        /* 规范 §2.6.4.2：所有设备可写描述符必须排在只读描述符之后。 */
        if (seen_writable && !(desc.flags & VRING_DESC_F_WRITE)) {
            *reason = "device-readable descriptor after device-writable one";
            return -1;
        }
        seen_writable |= (desc.flags & VRING_DESC_F_WRITE) != 0;
        chain[count] = desc;
        index[count] = idx;
        count++;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            return count;
        idx = desc.next;
    }
}

int virtq_walk_chain(const struct virtq *vq, uint16_t head, virtq_desc_fn fn, void *opaque)
{
    struct vring_desc chain[VIRTQ_MAX_SIZE];
    uint16_t index[VIRTQ_MAX_SIZE];
    const char *reason;
    int count = collect_chain(vq, head, chain, index, &reason);
    if (count < 0)
        return -1;
    for (int i = 0; fn && i < count; i++)
        if (fn(&chain[i], index[i], opaque) < 0)
            return -1;
    return count;
}

int virtq_used_push(struct virtq *vq, uint16_t head, uint32_t len)
{
    /* 只有“已取走、未完成”的链才能完成；这也保证 used ring 不会溢出。 */
    if (head >= vq->qsz || vq->last_seen_avail == vq->next_used_idx)
        return -1;
    vq->used->ring[slot(vq, vq->next_used_idx)] = (struct vring_used_elem){ .id = head, .len = len };
    vq->next_used_idx++;
    publish_idx(&vq->used->idx, vq->next_used_idx);
    return 0;
}

int virtq_needs_interrupt(const struct virtq *vq)
{
    return !(__atomic_load_n(&vq->avail->flags, __ATOMIC_ACQUIRE) & VRING_AVAIL_F_NO_INTERRUPT);
}

/* ---- 统计与调试 ---- */

unsigned int virtq_avail_count(const struct virtq *vq)
{
    return (uint16_t)(load_idx(&vq->avail->idx) - vq->last_seen_avail);
}

unsigned int virtq_used_count(const struct virtq *vq)
{
    return (uint16_t)(load_idx(&vq->used->idx) - vq->last_used_idx);
}

static const char *flag_names(uint16_t flags)
{
    static const char *const names[] = {
        "-", "NEXT", "WRITE", "NEXT|WRITE", "INDIRECT", "NEXT|INDIRECT",
        "WRITE|INDIRECT", "NEXT|WRITE|INDIRECT",
    };
    return flags > 7 ? "?" : names[flags];
}

void virtq_dump(const struct virtq *vq, FILE *out)
{
    fprintf(out, "--- Descriptor Table (qsz=%u) ---\n", vq->qsz);
    fprintf(out, "%-5s %-19s %-11s %-15s %s\n", "Idx", "Addr", "Len", "Flags", "Next");
    for (unsigned int i = 0; i < vq->qsz; i++) {
        const struct vring_desc *d = &vq->desc[i];
        fprintf(out, "%-5u 0x%016" PRIx64 "  %-11" PRIu32 " %-15s %u\n", i, d->addr, d->len,
                flag_names(d->flags), d->next);
    }

    unsigned int avail_count = virtq_avail_count(vq);
    fprintf(out, "--- Available Ring (driver -> device) ---\n");
    fprintf(out, "avail.flags = 0x%04x\n", vq->avail->flags);
    fprintf(out, "avail.idx   = %u\n", vq->avail->idx);
    fprintf(out, "avail.ring[] (device view: last_seen=%u, count=%u):\n", vq->last_seen_avail,
            avail_count);
    if (avail_count == 0)
        fprintf(out, "  (empty)\n");
    else if (avail_count > vq->qsz)
        fprintf(out, "  (corrupt: avail.idx is %u entries ahead, more than qsz)\n", avail_count);
    for (unsigned int i = 0; avail_count <= vq->qsz && i < avail_count; i++) {
        uint16_t s = slot(vq, (uint16_t)(vq->last_seen_avail + i));
        fprintf(out, "  avail[%u] -> desc chain head %u\n", s, vq->avail->ring[s]);
    }

    unsigned int used_count = virtq_used_count(vq);
    fprintf(out, "--- Used Ring (device -> driver) ---\n");
    fprintf(out, "used.flags  = 0x%04x\n", vq->used->flags);
    fprintf(out, "used.idx    = %u\n", vq->used->idx);
    fprintf(out, "used.ring[] (driver view: last_used=%u, count=%u):\n", vq->last_used_idx,
            used_count);
    if (used_count == 0)
        fprintf(out, "  (empty)\n");
    else if (used_count > vq->qsz)
        fprintf(out, "  (corrupt: used.idx is %u entries ahead, more than qsz)\n", used_count);
    for (unsigned int i = 0; used_count <= vq->qsz && i < used_count; i++) {
        uint16_t s = slot(vq, (uint16_t)(vq->last_used_idx + i));
        fprintf(out, "  used[%u] -> {id=%" PRIu32 ", len=%" PRIu32 "}\n", s,
                vq->used->ring[s].id, vq->used->ring[s].len);
    }
}

int virtq_dump_chain(const struct virtq *vq, uint16_t head, FILE *out)
{
    struct vring_desc chain[VIRTQ_MAX_SIZE];
    uint16_t index[VIRTQ_MAX_SIZE];
    const char *reason;
    int count = collect_chain(vq, head, chain, index, &reason);
    if (count < 0) {
        fprintf(out, "chain head %u: invalid (%s)\n", head, reason);
        return -1;
    }
    uint64_t readable = 0, writable = 0;
    for (int i = 0; i < count; i++) {
        if (chain[i].flags & VRING_DESC_F_WRITE)
            writable += chain[i].len;
        else
            readable += chain[i].len;
    }
    fprintf(out, "chain head %u: %d descriptors, %" PRIu64 " bytes device-readable, %" PRIu64
                 " bytes device-writable\n", head, count, readable, writable);
    for (int i = 0; i < count; i++)
        fprintf(out, "  desc[%u] %s %5" PRIu32 " bytes @ 0x%" PRIx64 "%s\n", index[i],
                chain[i].flags & VRING_DESC_F_WRITE ? "WO" : "RO", chain[i].len, chain[i].addr,
                i + 1 < count ? " ->" : "");
    return 0;
}

/*
 * virtq-demo：在一块模拟的客户机内存上演示 split virtqueue 的完整握手。
 *
 *   场景 1  virtio-blk 风格的读：header(RO) → 512 字节数据 buffer(WO)
 *   场景 2  scatter/gather 写：header(RO) → 3 段不连续的 64 字节数据(RO)
 *
 * driver 和 device 在同一个进程里，doorbell 和中断都是函数调用；但描述符里
 * 只有客户机物理地址（GPA），device 访问 buffer 前都要经 virtio_gpa() 检查，
 * 和 Stage 06 放进真实客户机内存后的做法一致。不需要 KVM 或 root。
 *
 * 请求头沿用 virtio-blk 的 16 字节布局（type/reserved/sector），但省略了链尾
 * 的 1 字节 status，完整协议留给 Stage 06。
 */
#include "virtio/virtio.h"
#include "virtio/virtq.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define GUEST_RAM_SIZE 0x10000u
#define QUEUE_SIZE 8u
#define SECTOR_SIZE 512u
#define DISK_SECTORS 16u
#define SEGMENT_SIZE 64u

#define VIRTIO_BLK_T_IN 0u
#define VIRTIO_BLK_T_OUT 1u

/* 客户机物理内存布局；三段写数据故意不相邻。 */
#define READ_HEADER_GPA 0x1000u
#define READ_DATA_GPA 0x2000u
#define WRITE_HEADER_GPA 0x3000u
static const uint64_t segment_gpa[3] = { 0x4000u, 0x5040u, 0x6080u };

struct blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};
_Static_assert(sizeof(struct blk_req_header) == 16, "virtio-blk header is 16 bytes");

struct sim_disk {
    uint8_t data[DISK_SECTORS * SECTOR_SIZE];
};

static uint8_t guest_ram[GUEST_RAM_SIZE];
static struct sim_disk disk;

static void disk_init(struct sim_disk *d)
{
    for (unsigned int s = 0; s < DISK_SECTORS; s++) {
        char unit[32];
        int n = snprintf(unit, sizeof(unit), "<disk sector %u>", s);
        for (unsigned int i = 0; i < SECTOR_SIZE; i++)
            d->data[s * SECTOR_SIZE + i] = (uint8_t)unit[i % (unsigned int)n];
    }
}

static void print_bytes(const char *prefix, const uint8_t *data, size_t len)
{
    printf("%s\"", prefix);
    for (size_t i = 0; i < len; i++)
        putchar(data[i] >= 0x20 && data[i] < 0x7f && data[i] != '"' ? data[i] : '.');
    printf("\"\n");
}

static void dump_state(const struct virtq *vq, const char *label)
{
    printf("\n--- virtq state: %s ---\n", label);
    virtq_dump(vq, stdout);
    printf("\n");
}

/* ---- device：一个极简的 virtio-blk 后端 ---- */

struct blk_request {
    struct vring_desc desc[QUEUE_SIZE];
    unsigned int count;
};

static int collect_desc(const struct vring_desc *desc, uint16_t idx, void *opaque)
{
    (void)idx;
    struct blk_request *req = opaque;
    req->desc[req->count++] = *desc;
    return 0;
}

/* 处理一条链；返回写进可写 buffer 的字节数，请求非法时返回 -1。 */
static int64_t blk_handle(struct virtio_device *dev, const struct blk_request *req)
{
    struct sim_disk *d = dev->opaque;
    const struct vring_desc *hd = &req->desc[0];
    if (hd->flags & VRING_DESC_F_WRITE || hd->len < sizeof(struct blk_req_header)) {
        printf("device: first descriptor is not a readable request header\n");
        return -1;
    }
    const void *hp = virtio_gpa(dev, hd->addr, sizeof(struct blk_req_header));
    if (!hp) {
        printf("device: request header @0x%" PRIx64 " is outside guest memory\n", hd->addr);
        return -1;
    }
    /* 拷贝一份再用：客户机可能在 device 读取期间改写共享内存。 */
    struct blk_req_header header;
    memcpy(&header, hp, sizeof(header));
    if (header.sector >= DISK_SECTORS) {
        printf("device: sector %" PRIu64 " is beyond the %u-sector disk\n", header.sector,
               DISK_SECTORS);
        return -1;
    }
    uint64_t offset = header.sector * SECTOR_SIZE;
    uint64_t moved = 0;
    for (unsigned int i = 1; i < req->count; i++) {
        const struct vring_desc *b = &req->desc[i];
        int writable = (b->flags & VRING_DESC_F_WRITE) != 0;
        if (writable != (header.type == VIRTIO_BLK_T_IN)) {
            printf("device: descriptor %u has the wrong direction for this request\n", i);
            return -1;
        }
        if (b->len > sizeof(d->data) - offset) {
            printf("device: request runs past the end of the disk\n");
            return -1;
        }
        uint8_t *buf = virtio_gpa(dev, b->addr, b->len);
        if (!buf) {
            printf("device: buffer @0x%" PRIx64 "+%" PRIu32 " is outside guest memory\n",
                   b->addr, b->len);
            return -1;
        }
        if (writable)
            memcpy(buf, d->data + offset, b->len);
        else
            memcpy(d->data + offset, buf, b->len);
        offset += b->len;
        moved += b->len;
    }
    switch (header.type) {
    case VIRTIO_BLK_T_IN:
        printf("device: IN  sector %" PRIu64 ", scattered %" PRIu64
               " bytes into %u buffer(s)\n", header.sector, moved, req->count - 1);
        return (int64_t)moved;
    case VIRTIO_BLK_T_OUT:
        printf("device: OUT sector %" PRIu64 ", gathered %" PRIu64
               " bytes from %u segment(s)\n", header.sector, moved, req->count - 1);
        return 0; /* 写请求不写客户机 buffer */
    default:
        printf("device: unsupported request type %" PRIu32 "\n", header.type);
        return -1;
    }
}

static void blk_on_notify(struct virtio_device *dev)
{
    printf("device: doorbell #%" PRIu64 " (in a real VMM: MMIO write -> VM exit)\n",
           dev->notifications);
}

static int blk_on_process(struct virtio_device *dev)
{
    struct virtq *vq = dev->vq;
    uint16_t head;
    int rc;
    while ((rc = virtq_avail_consume(vq, &head)) == 1) {
        printf("device: consumed avail entry -> chain head %u\n", head);
        if (virtq_dump_chain(vq, head, stdout) < 0)
            return -1;
        struct blk_request req = { .count = 0 };
        if (virtq_walk_chain(vq, head, collect_desc, &req) < 0)
            return -1;
        int64_t written = blk_handle(dev, &req);
        if (written < 0)
            return -1;
        if (virtq_used_push(vq, head, (uint32_t)written) < 0)
            return -1;
        printf("device: used.ring <- {id=%u, len=%" PRId64 "}, used.idx = %u\n", head, written,
               vq->used->idx);
    }
    if (rc < 0)
        printf("device: avail.idx moved by more than the queue size\n");
    return rc;
}

static void blk_on_interrupt(struct virtio_device *dev)
{
    printf("IRQ: used-buffer interrupt #%" PRIu64 " (used.idx=%u)\n", dev->interrupts,
           dev->vq->used->idx);
}

static const struct virtio_device_ops blk_ops = {
    .on_notify = blk_on_notify,
    .on_process = blk_on_process,
    .on_interrupt = blk_on_interrupt,
};

/* ---- driver：客户机一侧 ---- */

static int driver_submit_and_kick(struct virtio_device *dev, uint16_t head)
{
    struct virtq *vq = dev->vq;
    if (virtq_avail_submit(vq, head) < 0) {
        fprintf(stderr, "driver: avail ring is full\n");
        return -1;
    }
    printf("driver: avail.ring[%u] = %u, avail.idx = %u\n",
           (uint16_t)(vq->avail->idx - 1) & (vq->qsz - 1), head, vq->avail->idx);
    dump_state(vq, "after driver submit");
    if (virtq_needs_notify(vq) && virtio_device_kick(dev) < 0) {
        fprintf(stderr, "device: request failed, device needs reset\n");
        return -1;
    }
    dump_state(vq, "after device completes");
    return 0;
}

static int driver_reclaim(struct virtq *vq, struct vring_used_elem *elem)
{
    if (virtq_used_pop(vq, elem) != 1) {
        fprintf(stderr, "driver: no valid used entry\n");
        return -1;
    }
    printf("driver: used elem {id=%" PRIu32 ", len=%" PRIu32 "}; chain %" PRIu32
           " can be reused\n", elem->id, elem->len, elem->id);
    return 0;
}

static int scenario_read(struct virtio_device *dev)
{
    struct virtq *vq = dev->vq;
    printf("=== Scenario 1: virtio-blk read, sector 3 (header RO + data WO) ===\n");
    const struct blk_req_header header = { .type = VIRTIO_BLK_T_IN, .sector = 3 };
    memcpy(guest_ram + READ_HEADER_GPA, &header, sizeof(header));
    memset(guest_ram + READ_DATA_GPA, 0, SECTOR_SIZE);

    if (virtq_desc_fill(vq, 0, READ_HEADER_GPA, sizeof(header), VRING_DESC_F_NEXT, 1) < 0 ||
        virtq_desc_fill(vq, 1, READ_DATA_GPA, SECTOR_SIZE, VRING_DESC_F_WRITE, 0) < 0)
        return -1;
    printf("driver: desc[0] = header @0x%x, desc[1] = data buffer @0x%x\n", READ_HEADER_GPA,
           READ_DATA_GPA);
    if (driver_submit_and_kick(dev, 0) < 0)
        return -1;

    struct vring_used_elem elem;
    if (driver_reclaim(vq, &elem) < 0)
        return -1;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "driver: data buffer @0x%x = ", READ_DATA_GPA);
    print_bytes(prefix, guest_ram + READ_DATA_GPA, elem.len < 64 ? elem.len : 64);
    dump_state(vq, "after driver reclaims");
    return 0;
}

static int scenario_scatter_gather_write(struct virtio_device *dev)
{
    struct virtq *vq = dev->vq;
    printf("=== Scenario 2: scatter/gather write, sector 5 (header + 3 segments) ===\n");
    const struct blk_req_header header = { .type = VIRTIO_BLK_T_OUT, .sector = 5 };
    memcpy(guest_ram + WRITE_HEADER_GPA, &header, sizeof(header));
    for (unsigned int i = 0; i < 3; i++)
        memset(guest_ram + segment_gpa[i], 'A' + (int)i, SEGMENT_SIZE);

    /* 复用场景 1 已回收的描述符 0 和 1。 */
    if (virtq_desc_fill(vq, 0, WRITE_HEADER_GPA, sizeof(header), VRING_DESC_F_NEXT, 1) < 0)
        return -1;
    for (uint16_t i = 0; i < 3; i++) {
        uint16_t idx = (uint16_t)(i + 1);
        uint16_t flags = i < 2 ? VRING_DESC_F_NEXT : 0;
        if (virtq_desc_fill(vq, idx, segment_gpa[i], SEGMENT_SIZE, flags,
                            (uint16_t)(i < 2 ? idx + 1 : 0)) < 0)
            return -1;
    }
    printf("driver: desc[0] = header @0x%x, desc[1..3] = segments @0x%" PRIx64 ", 0x%" PRIx64
           ", 0x%" PRIx64 "\n", WRITE_HEADER_GPA, segment_gpa[0], segment_gpa[1],
           segment_gpa[2]);
    if (driver_submit_and_kick(dev, 0) < 0)
        return -1;

    struct vring_used_elem elem;
    if (driver_reclaim(vq, &elem) < 0)
        return -1;
    print_bytes("host: disk sector 5 [0..256) = ", disk.data + 5 * SECTOR_SIZE, 256);
    dump_state(vq, "after driver reclaims");
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    disk_init(&disk);
    struct virtio_device *dev =
        virtio_device_create(VIRTIO_ID_BLOCK, QUEUE_SIZE, guest_ram, sizeof(guest_ram));
    if (!dev) {
        fprintf(stderr, "virtq-demo: cannot create device\n");
        return 1;
    }
    virtio_device_set_ops(dev, &blk_ops, &disk);
    printf("%s: 1 split virtqueue, qsz=%u, guest RAM %u KiB (simulated, no KVM)\n\n",
           virtio_device_name(dev->device_id), QUEUE_SIZE, GUEST_RAM_SIZE / 1024);

    int rc = scenario_read(dev);
    if (rc == 0)
        rc = scenario_scatter_gather_write(dev);
    if (rc == 0)
        printf("summary: %" PRIu64 " doorbells, %" PRIu64 " interrupts, avail.idx=%u, "
               "used.idx=%u\n", dev->notifications, dev->interrupts, dev->vq->avail->idx,
               dev->vq->used->idx);
    virtio_device_destroy(dev);
    return rc == 0 ? 0 : 1;
}

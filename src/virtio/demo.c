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
 *
 * 一次完整握手（每个场景都按这个顺序走一遍）：
 *
 *   driver  1. 把请求头和数据 buffer 放进客户机内存
 *           2. 用描述符把这些 buffer 串成一条链   virtq_desc_fill()
 *           3. 把链头挂到 avail ring 并发布       virtq_avail_submit()
 *           4. 按需 kick device                   virtq_needs_notify() + virtio_device_kick()
 *   device  5. 从 avail ring 取链头               virtq_avail_consume()
 *           6. 校验并遍历整条链                   virtq_walk_chain()
 *           7. 按请求读写 buffer（模拟磁盘 I/O）  blk_handle()
 *           8. 把 {链头, 写入字节数} 放进 used ring  virtq_used_push()
 *           9. 按需发中断                         virtq_needs_interrupt()
 *   driver 10. 从 used ring 回收完成项，链上的描述符可以复用了  virtq_used_pop()
 *
 * 每个关键步骤后都会 dump 一次队列状态，可以对照输出看各个 idx 怎么变化。
 */
#include "virtio/virtio.h"
#include "virtio/virtq.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define GUEST_RAM_SIZE 0x10000u /* 模拟的客户机内存：64 KiB，GPA 范围 [0, 0x10000) */
#define QUEUE_SIZE 8u           /* 队列深度，必须是 2 的幂 */
#define SECTOR_SIZE 512u
#define DISK_SECTORS 16u        /* 模拟磁盘：16 个扇区，共 8 KiB */
#define SEGMENT_SIZE 64u        /* 场景 2 里每段写数据的长度 */

/* virtio-blk 请求类型。方向是从 driver 的角度说的：
 * IN  = 数据流进客户机（读盘），device 写客户机 buffer；
 * OUT = 数据流出客户机（写盘），device 读客户机 buffer。 */
#define VIRTIO_BLK_T_IN 0u
#define VIRTIO_BLK_T_OUT 1u

/*
 * 客户机物理内存布局（都是 GPA，也就是 guest_ram 里的偏移）：
 *
 *   0x1000  场景 1 请求头      0x4000  场景 2 第 1 段数据
 *   0x2000  场景 1 读缓冲区    0x5040  场景 2 第 2 段数据
 *   0x3000  场景 2 请求头      0x6080  场景 2 第 3 段数据
 *
 * 三段写数据故意不相邻、也不按页对齐，用来演示 scatter/gather：
 * 客户机内存里分散的几块，由一条描述符链拼成一次连续的磁盘写。
 */
#define READ_HEADER_GPA 0x1000u
#define READ_DATA_GPA 0x2000u
#define WRITE_HEADER_GPA 0x3000u
static const uint64_t segment_gpa[3] = { 0x4000u, 0x5040u, 0x6080u };

/* virtio-blk 请求头，总是链里的第一个描述符，对 device 只读。 */
struct blk_req_header {
    uint32_t type;     /* VIRTIO_BLK_T_IN / VIRTIO_BLK_T_OUT */
    uint32_t reserved;
    uint64_t sector;   /* 起始扇区号，以 512 字节为单位 */
};
_Static_assert(sizeof(struct blk_req_header) == 16, "virtio-blk header is 16 bytes");

/* 宿主侧的“磁盘”，就是一块普通内存；客户机看不到它，只能通过 device 读写。 */
struct sim_disk {
    uint8_t data[DISK_SECTORS * SECTOR_SIZE];
};

/* 客户机内存：描述符里的 GPA x 对应 guest_ram[x]。 */
static uint8_t guest_ram[GUEST_RAM_SIZE];
static struct sim_disk disk;

/* 每个扇区重复填 "<disk sector N>"，这样从读出的内容就能看出读的是哪个扇区。 */
static void disk_init(struct sim_disk *d)
{
    for (unsigned int s = 0; s < DISK_SECTORS; s++) {
        char unit[32];
        int n = snprintf(unit, sizeof(unit), "<disk sector %u>", s);
        for (unsigned int i = 0; i < SECTOR_SIZE; i++)
            d->data[s * SECTOR_SIZE + i] = (uint8_t)unit[i % (unsigned int)n];
    }
}

/* 把一段字节按字符串打印，不可打印字符和双引号显示成 '.'。 */
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

/*
 * 一条描述符链的快照。desc[0] 是请求头，desc[1..count-1] 是数据 buffer。
 * 一条合法的链不会成环，所以最多 QUEUE_SIZE 项，数组不会越界。
 */
struct blk_request {
    struct vring_desc desc[QUEUE_SIZE];
    unsigned int count;
};

/*
 * virtq_walk_chain() 的回调：把链上每个描述符拷一份到 blk_request 里。
 * 拷贝而不是直接引用：描述符表在共享内存里，客户机随时可能改写，
 * device 校验之后应当只用自己手里的副本。
 */
static int collect_desc(const struct vring_desc *desc, uint16_t idx, void *opaque)
{
    (void)idx;
    struct blk_request *req = opaque;
    req->desc[req->count++] = *desc;
    return 0;
}

/*
 * 处理一条链；返回写进可写 buffer 的字节数，请求非法时返回 -1。
 * 这个返回值就是之后放进 used ring 的 len，driver 靠它知道读回了多少数据。
 *
 * 链里的地址、长度、请求头内容全部由客户机控制，所以每一项都要先检查再使用：
 * 请求头必须可读且够长，扇区号不能越过磁盘，每个 buffer 的方向要和请求类型一致，
 * 数据不能写出磁盘末尾，每个 GPA 都要经过 virtio_gpa() 检查是否越界。
 */
static int64_t blk_handle(struct virtio_device *dev, const struct blk_request *req)
{
    struct sim_disk *d = dev->opaque;

    /* 第 1 步：解析请求头。它必须是只读的（driver 写、device 读），且至少 16 字节。 */
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
    /*
     * 第 2 步：依次处理后面的数据 buffer。这些 buffer 在客户机内存里可以不连续，
     * 但在磁盘上是连续的：offset 从起始扇区开始，每处理一段就往后推进。
     */
    uint64_t offset = header.sector * SECTOR_SIZE; /* 当前磁盘位置 */
    uint64_t moved = 0;                            /* 已搬运的总字节数 */
    for (unsigned int i = 1; i < req->count; i++) {
        const struct vring_desc *b = &req->desc[i];
        /* 读盘（IN）时 device 要往 buffer 里写，所以 buffer 必须可写；写盘（OUT）时相反。 */
        int writable = (b->flags & VRING_DESC_F_WRITE) != 0;
        if (writable != (header.type == VIRTIO_BLK_T_IN)) {
            printf("device: descriptor %u has the wrong direction for this request\n", i);
            return -1;
        }
        /* 写成减法防止溢出；offset 不会超过磁盘大小（扇区号已检查，且每次推进前都过了这一步）。 */
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
        /* 真正的“I/O”：IN 把磁盘内容拷进客户机 buffer（scatter），
         * OUT 把客户机 buffer 拷到磁盘（gather）。 */
        if (writable)
            memcpy(buf, d->data + offset, b->len);
        else
            memcpy(d->data + offset, buf, b->len);
        offset += b->len;
        moved += b->len;
    }

    /* 第 3 步：决定 used ring 里的 len。只统计 device 写进客户机的字节数。 */
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

/*
 * 下面三个函数是 virtio_device_kick() 依次调用的回调：
 * on_notify → on_process → （有完成项且 driver 没关中断时）on_interrupt。
 */

/* 收到 doorbell。这里只打印；真正的活在 on_process 里做。 */
static void blk_on_notify(struct virtio_device *dev)
{
    printf("device: doorbell #%" PRIu64 " (in a real VMM: MMIO write -> VM exit)\n",
           dev->notifications);
}

/*
 * device 的主循环：把 avail ring 里所有待处理的链一条条处理完。
 * 一次 doorbell 可能对应多条请求（driver 可以连续提交几条再 kick 一次），
 * 所以要循环到 avail ring 取空为止，而不是只处理一条。
 *
 * 返回 0 表示全部处理完；返回 -1 表示遇到非法请求或 driver 的 avail.idx 异常，
 * 设备需要 reset。出错前已经完成的条目已经写进 used ring，kick 仍会为它们发中断。
 */
static int blk_on_process(struct virtio_device *dev)
{
    struct virtq *vq = dev->vq;
    uint16_t head;
    int rc;
    /* consume 返回 1：取到一个链头；0：avail ring 空了；-1：avail.idx 非法。 */
    while ((rc = virtq_avail_consume(vq, &head)) == 1) {
        printf("device: consumed avail entry -> chain head %u\n", head);
        if (virtq_dump_chain(vq, head, stdout) < 0)
            return -1;
        /* walk_chain 先校验整条链（越界、成环等），通过后才逐项调用 collect_desc。 */
        struct blk_request req = { .count = 0 };
        if (virtq_walk_chain(vq, head, collect_desc, &req) < 0)
            return -1;
        int64_t written = blk_handle(dev, &req);
        if (written < 0)
            return -1;
        /* 告诉 driver：以 head 开头的链完成了，写入了 written 字节。 */
        if (virtq_used_push(vq, head, (uint32_t)written) < 0)
            return -1;
        printf("device: used.ring <- {id=%u, len=%" PRId64 "}, used.idx = %u\n", head, written,
               vq->used->idx);
    }
    if (rc < 0)
        printf("device: avail.idx moved by more than the queue size\n");
    return rc;
}

/* 模拟给客户机注入中断（真实 VMM 中是写 irqfd）。driver 收到后去 used ring 回收。 */
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

/*
 * driver 提交一条请求的标准两步：把描述符链挂到 avail ring 上，再按需 kick device。
 * 调用前，以 head 开头的描述符链必须已经用 virtq_desc_fill() 填好。
 *
 *   1. virtq_avail_submit()  把 head 写进 avail.ring，再推进 avail.idx。
 *                            推进 idx 之后 device 才能看到这条请求，也就是“发布”。
 *   2. virtq_needs_notify()  device 没有设置 NO_NOTIFY 时才 kick；
 *                            设置了就说明它正在主动处理 avail ring，自己会看到新条目。
 *   3. virtio_device_kick()  模拟写 doorbell。
 *
 * 这里 kick 是同步函数调用，返回时 device 已经处理完并写好 used ring，所以可以
 * 紧接着打印 “after device completes”。真实 VMM 中 kick 只是一次 MMIO 写，会立刻返回，
 * 完成是异步的，由之后的中断通知 driver。本 demo 的 device 从不设置 NO_NOTIFY，
 * 所以一定会走到 kick。
 *
 * 这个函数不回收完成项，回收由调用方随后通过 driver_reclaim() 完成。
 * avail ring 已满，或 device 处理出错（规范里的 DEVICE_NEEDS_RESET）时返回 -1。
 */
static int driver_submit_and_kick(struct virtio_device *dev, uint16_t head)
{
    struct virtq *vq = dev->vq;
    if (virtq_avail_submit(vq, head) < 0) {
        fprintf(stderr, "driver: avail ring is full\n");
        return -1;
    }
    /* avail.idx 是一直递增、到 65535 后回绕的计数器，不是槽位号。刚写入的槽位是
     * (idx - 1) mod qsz；qsz 是 2 的幂，所以取模可以写成 & (qsz - 1)。 */
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

/*
 * driver 从 used ring 回收一条完成项。elem->id 是完成的链头，elem->len 是
 * device 写进可写 buffer 的字节数。回收之后，这条链上的描述符重新归 driver 所有，
 * 可以拿去填下一个请求（场景 2 就复用了场景 1 的描述符 0 和 1）。
 * demo 每次只提交一条请求，所以这里一定应该恰好取到一条；取不到就算出错。
 */
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

/*
 * 场景 1：从扇区 3 读 512 字节。描述符链有两项：
 *
 *   desc[0]  请求头 @0x1000, 16 字节, 只读  --NEXT-->  desc[1]  数据 @0x2000, 512 字节, 可写
 *
 * 读请求把只读的请求头放在前面、可写的数据 buffer 放在后面，这是规范要求的顺序。
 */
static int scenario_read(struct virtio_device *dev)
{
    struct virtq *vq = dev->vq;
    printf("=== Scenario 1: virtio-blk read, sector 3 (header RO + data WO) ===\n");
    /* 1. driver 在客户机内存里写好请求头，并把数据 buffer 清零，方便看出 device 写了什么。 */
    const struct blk_req_header header = { .type = VIRTIO_BLK_T_IN, .sector = 3 };
    memcpy(guest_ram + READ_HEADER_GPA, &header, sizeof(header));
    memset(guest_ram + READ_DATA_GPA, 0, SECTOR_SIZE);

    /* 2. 填描述符。参数依次是：下标、GPA、长度、flags、next。
     *    desc[0] 带 NEXT，指向 desc[1]；desc[1] 带 WRITE 且没有 NEXT，是链尾。 */
    if (virtq_desc_fill(vq, 0, READ_HEADER_GPA, sizeof(header), VRING_DESC_F_NEXT, 1) < 0 ||
        virtq_desc_fill(vq, 1, READ_DATA_GPA, SECTOR_SIZE, VRING_DESC_F_WRITE, 0) < 0)
        return -1;
    printf("driver: desc[0] = header @0x%x, desc[1] = data buffer @0x%x\n", READ_HEADER_GPA,
           READ_DATA_GPA);
    /* 3. 提交链头 0 并 kick；返回时 device 已经处理完。 */
    if (driver_submit_and_kick(dev, 0) < 0)
        return -1;

    /* 4. 回收完成项。elem.len 应为 512，数据 buffer 里应该是 "<disk sector 3>" 的重复。 */
    struct vring_used_elem elem;
    if (driver_reclaim(vq, &elem) < 0)
        return -1;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "driver: data buffer @0x%x = ", READ_DATA_GPA);
    print_bytes(prefix, guest_ram + READ_DATA_GPA, elem.len < 64 ? elem.len : 64);
    dump_state(vq, "after driver reclaims");
    return 0;
}

/*
 * 场景 2：把客户机内存里不相邻的 3 段数据写到扇区 5 开头。描述符链有四项：
 *
 *   desc[0] 请求头 @0x3000 → desc[1] 'A'x64 @0x4000 → desc[2] 'B'x64 @0x5040
 *                          → desc[3] 'C'x64 @0x6080（链尾）
 *
 * 全部只读：写盘时 device 只读客户机内存。device 按链的顺序把它们拼起来，
 * 写到磁盘的连续位置，所以扇区 5 的前 192 字节会变成 AAA…BBB…CCC…。
 */
static int scenario_scatter_gather_write(struct virtio_device *dev)
{
    struct virtq *vq = dev->vq;
    printf("=== Scenario 2: scatter/gather write, sector 5 (header + 3 segments) ===\n");
    const struct blk_req_header header = { .type = VIRTIO_BLK_T_OUT, .sector = 5 };
    memcpy(guest_ram + WRITE_HEADER_GPA, &header, sizeof(header));
    for (unsigned int i = 0; i < 3; i++)
        memset(guest_ram + segment_gpa[i], 'A' + (int)i, SEGMENT_SIZE);

    /* 复用场景 1 已回收的描述符 0 和 1，再加上 2、3。 */
    if (virtq_desc_fill(vq, 0, WRITE_HEADER_GPA, sizeof(header), VRING_DESC_F_NEXT, 1) < 0)
        return -1;
    /* desc[1..3] 是三段数据：前两段带 NEXT 指向下一项，最后一段 flags=0，表示链结束。 */
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

    /* 写请求 device 不往客户机写数据，所以 elem.len 为 0。
     * 打印前 256 字节：前 192 字节是写入的 A/B/C，后面还是原来的 "<disk sector 5>"。 */
    struct vring_used_elem elem;
    if (driver_reclaim(vq, &elem) < 0)
        return -1;
    print_bytes("host: disk sector 5 [0..256) = ", disk.data + 5 * SECTOR_SIZE, 256);
    dump_state(vq, "after driver reclaims");
    return 0;
}

int main(void)
{
    /* 行缓冲：stdout 被重定向到管道或文件时，输出也能和 stderr 按时间顺序交错。 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    disk_init(&disk);
    /* 建一个 virtio-blk 设备：一个 8 项的队列，客户机内存就是 guest_ram。 */
    struct virtio_device *dev =
        virtio_device_create(VIRTIO_ID_BLOCK, QUEUE_SIZE, guest_ram, sizeof(guest_ram));
    if (!dev) {
        fprintf(stderr, "virtq-demo: cannot create device\n");
        return 1;
    }
    /* 挂上 blk 后端的三个回调；opaque 传磁盘，blk_handle 里通过 dev->opaque 拿到它。 */
    virtio_device_set_ops(dev, &blk_ops, &disk);
    printf("%s: 1 split virtqueue, qsz=%u, guest RAM %u KiB (simulated, no KVM)\n\n",
           virtio_device_name(dev->device_id), QUEUE_SIZE, GUEST_RAM_SIZE / 1024);

    int rc = scenario_read(dev);
    if (rc == 0)
        rc = scenario_scatter_gather_write(dev);
    /* 两个场景各一次 doorbell、一次中断，avail.idx 和 used.idx 应该都是 2。 */
    if (rc == 0)
        printf("summary: %" PRIu64 " doorbells, %" PRIu64 " interrupts, avail.idx=%u, "
               "used.idx=%u\n", dev->notifications, dev->interrupts, dev->vq->avail->idx,
               dev->vq->used->idx);
    virtio_device_destroy(dev);
    return rc == 0 ? 0 : 1;
}

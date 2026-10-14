#include "virtio/blk.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* 每次启动最多打印这么多条请求错误，防止恶意客户机用日志刷爆宿主。 */
#define BLK_ERROR_LOG_LIMIT 16u

/*
 * 一个读请求在客户机内存里的样子（例如 Linux 读 8 个扇区到两个不相邻的页）：
 *
 *   descriptor table（客户机内存）              客户机 RAM 里的 buffer
 *   ┌────┬───────────┬──────┬───────────┐
 *   │ #5 │ 0x1234000 │   16 │ NEXT      │ ──► { type=IN, reserved, sector=2048 }
 *   │ #6 │ 0x2000000 │ 2048 │ NEXT|WRITE│ ──► [ 2 KiB 空 buffer，VMM 写入 ]
 *   │ #7 │ 0x3000800 │ 2048 │ NEXT|WRITE│ ──► [ 2 KiB 空 buffer，VMM 写入 ]
 *   │ #8 │ 0x1234010 │    1 │ WRITE     │ ──► [ status：VMM 写 0/1/2 ]
 *   └────┴───────────┴──────┴───────────┘
 *   avail.ring[k] = 5，avail.idx++，写 QueueNotify
 *
 * Linux 侧对应 drivers/block/virtio_blk.c 的 virtio_queue_rq() → virtblk_add_req()：
 * 把请求头、bio 的各个段、状态字节分别做成 scatterlist，交给 virtqueue_add_sgs()，
 * 它负责挑空闲描述符、串成链、写 avail ring。
 *
 * VMM 侧的处理流程：
 *
 *   blk_process_queue()         把 avail ring 里新出现的链一条条取出来
 *     blk_handle_request()      处理一条链
 *       virtq_walk_chain()      校验链结构，拷贝成快照 struct chain
 *       找状态字节、读请求头
 *       blk_rw() / flush / GET_ID
 *       写状态字节
 *     virtq_used_push()         写 used ring：{id = 链头, len = 写给客户机的字节数}
 *   （回到 mmio.c 的 queue_notify() 发中断）
 *
 * 数据直接在客户机内存和镜像文件之间 pread/pwrite，中间没有额外拷贝：
 * buf 就是 GPA 换算出来的宿主指针。
 */

/*
 * 一条描述符链的快照。virtq_walk_chain() 已经检查过下标、环、INDIRECT 和
 * 读写顺序，并且回调拿到的是它的私有拷贝，所以之后客户机再改描述符表也
 * 不会影响这里（避免 double fetch）。
 *
 * double fetch：VMM 第一次读 desc.len 做检查，第二次读 desc.len 去用；客户机的
 * 另一个 vCPU 在两次读之间把它改大，检查就被绕过了。解决办法是只读一次、存下来。
 */
struct chain {
    struct vring_desc desc[VIRTQ_MAX_SIZE];
    unsigned int count;
};

/* virtq_walk_chain() 的回调：把每个描述符依次存进快照。 */
static int collect_desc(const struct vring_desc *desc, uint16_t idx, void *opaque)
{
    (void)idx;
    struct chain *chain = opaque;
    chain->desc[chain->count++] = *desc;
    return 0;
}

__attribute__((format(printf, 2, 3)))
static void blk_error(struct virtio_blk *blk, const char *format, ...)
{
    blk->stats.errors++;
    if (blk->logged_errors >= BLK_ERROR_LOG_LIMIT)
        return;
    if (++blk->logged_errors == BLK_ERROR_LOG_LIMIT) {
        fprintf(stderr, "virtio-blk: further request errors are not logged\n");
        return;
    }
    va_list args;
    va_start(args, format);
    fputs("virtio-blk: request rejected: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

/*
 * 完整读写：pread/pwrite 可能只完成一部分，循环到结束；遇到 EOF 视为失败。
 * pread 返回 0 表示到了文件末尾（镜像在运行中被截短），这时 errno 没有意义，
 * 清成 0 让调用方打印 "end of file" 而不是一个无关的旧错误。
 * 用 pread/pwrite 而不是 lseek + read：自带偏移，不改文件位置，将来多个
 * 线程并发处理请求也不会互相干扰。
 */
static int pread_full(int fd, uint8_t *buf, uint64_t len, uint64_t offset)
{
    while (len) {
        ssize_t n = pread(fd, buf, len, (off_t)offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            errno = 0;
        if (n <= 0)
            return -1;
        buf += n;
        len -= (uint64_t)n;
        offset += (uint64_t)n;
    }
    return 0;
}

static int pwrite_full(int fd, const uint8_t *buf, uint64_t len, uint64_t offset)
{
    while (len) {
        ssize_t n = pwrite(fd, buf, len, (off_t)offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            errno = 0;
        if (n <= 0)
            return -1;
        buf += n;
        len -= (uint64_t)n;
        offset += (uint64_t)n;
    }
    return 0;
}

/*
 * 处理读写请求。所有检查都在第一次 I/O 之前完成：只要有一项不合法就整个拒绝，
 * 不会出现“前几段已经写进镜像，后一段才发现越界”的半完成请求。
 * *written 返回写进客户机内存的数据字节数。
 */
static uint8_t blk_rw(struct virtio_blk *blk, const struct chain *chain, uint32_t type,
                      uint64_t sector, uint64_t *written)
{
    struct virtio_mmio *dev = &blk->mmio;
    bool is_read = type == VIRTIO_BLK_T_IN;
    unsigned int first = 1, last = chain->count - 1; /* 数据段是 [first, last) */
    uint64_t total = 0;

    for (unsigned int i = first; i < last; i++) {
        const struct vring_desc *d = &chain->desc[i];
        /* 防线 1：方向。读盘时 device 写客户机 buffer，driver 必须把它标成可写。 */
        if (is_read != ((d->flags & VRING_DESC_F_WRITE) != 0)) {
            blk_error(blk, "%s data descriptor %u is %s", is_read ? "read" : "write", i,
                      is_read ? "read-only" : "device-writable");
            return VIRTIO_BLK_S_IOERR;
        }
        /* 防线 2：每段 buffer 都完整落在客户机 RAM 里（含 addr + len 回绕）。 */
        if (!virtio_mmio_gpa(dev, d->addr, d->len)) {
            blk_error(blk, "data buffer [0x%llx, +0x%x) is outside guest memory",
                      (unsigned long long)d->addr, d->len);
            return VIRTIO_BLK_S_IOERR;
        }
        total += d->len;
    }
    /* 防线 3：长度是整扇区，且至少一个扇区。 */
    if (total == 0 || total % VIRTIO_BLK_SECTOR_SIZE) {
        blk_error(blk, "data length %llu is not a positive multiple of %u",
                  (unsigned long long)total, VIRTIO_BLK_SECTOR_SIZE);
        return VIRTIO_BLK_S_IOERR;
    }
    /*
     * 防线 4：[sector, sector + total/512) 在磁盘内。写成与 capacity 的减法比较，
     * 不计算 sector * 512 或 sector + n，客户机给多大的 sector 都不会溢出。
     */
    uint64_t capacity = blk->config.capacity;
    uint64_t nsectors = total / VIRTIO_BLK_SECTOR_SIZE;
    if (sector > capacity || nsectors > capacity - sector) {
        blk_error(blk, "sectors [%llu, +%llu) are beyond the disk end (%llu sectors)",
                  (unsigned long long)sector, (unsigned long long)nsectors,
                  (unsigned long long)capacity);
        return VIRTIO_BLK_S_IOERR;
    }

    /* 防线 5：完整 I/O。短读（镜像被外部截短）不能当成功返回。 */
    uint64_t offset = sector * VIRTIO_BLK_SECTOR_SIZE; /* 上面已保证 sector < capacity，不会溢出 */
    /* 多个数据段在磁盘上是连续的：第一段从 offset 开始，下一段接着上一段的末尾。 */
    for (unsigned int i = first; i < last; i++) {
        const struct vring_desc *d = &chain->desc[i];
        uint8_t *buf = virtio_mmio_gpa(dev, d->addr, d->len);
        int rc = is_read ? pread_full(blk->fd, buf, d->len, offset)
                         : pwrite_full(blk->fd, buf, d->len, offset);
        if (rc < 0) {
            blk_error(blk, "%s at byte %llu failed: %s", is_read ? "read" : "write",
                      (unsigned long long)offset, errno ? strerror(errno) : "end of file");
            return VIRTIO_BLK_S_IOERR;
        }
        if (is_read)
            *written += d->len;
        offset += d->len;
    }
    if (is_read) {
        blk->stats.reads++;
        blk->stats.bytes_read += total;
    } else {
        blk->stats.writes++;
        blk->stats.bytes_written += total;
    }
    return VIRTIO_BLK_S_OK;
}

/*
 * GET_ID：客户机读设备序列号（Linux 在 /sys/block/vda/serial 里显示）。
 * 链是 头 + 一个可写的 20 字节 buffer + 状态。
 */
static uint8_t blk_get_id(struct virtio_blk *blk, const struct chain *chain, uint64_t *written)
{
    const struct vring_desc *d = &chain->desc[1];
    if (chain->count < 3 || !(d->flags & VRING_DESC_F_WRITE)) {
        blk_error(blk, "GET_ID needs a device-writable buffer");
        return VIRTIO_BLK_S_IOERR;
    }
    uint32_t len = d->len < VIRTIO_BLK_ID_BYTES ? d->len : VIRTIO_BLK_ID_BYTES;
    uint8_t *buf = virtio_mmio_gpa(&blk->mmio, d->addr, len);
    if (!buf) {
        blk_error(blk, "GET_ID buffer is outside guest memory");
        return VIRTIO_BLK_S_IOERR;
    }
    /* 序列号不足 20 字节时补 0，正好 20 字节时没有结尾 NUL（规范如此）。 */
    char id[VIRTIO_BLK_ID_BYTES + 1] = { 0 };
    snprintf(id, sizeof(id), "svmm-vda");
    memcpy(buf, id, len);
    *written += len;
    return VIRTIO_BLK_S_OK;
}

/*
 * 处理一条链，返回写进 used ring 的 len：写给客户机的总字节数（数据 + 状态字节）。
 * 链本身非法、找不到可写的状态字节时返回 0；这条链仍然会被完成，
 * driver 不会永远等待，但它看不到状态（Linux 会把请求当作 I/O 错误）。
 */
static uint32_t blk_handle_request(struct virtio_blk *blk, struct virtq *vq, uint16_t head)
{
    struct virtio_mmio *dev = &blk->mmio;
    struct chain chain = { .count = 0 };
    if (virtq_walk_chain(vq, head, collect_desc, &chain) < 0) {
        blk_error(blk, "invalid descriptor chain at head %u", head);
        return 0;
    }
    blk->stats.requests++;

    /*
     * 状态在最后一个描述符的最后一个字节，必须对 device 可写。
     * 规范只规定“链的最后一个字节是状态”，没规定它独占一个描述符；Linux 恰好
     * 给它单独一个 1 字节描述符，但别的 driver 可以把它和最后一段数据放在一起。
     * 先找状态字节，后面的任何错误才有地方报告给 driver。
     */
    const struct vring_desc *sd = &chain.desc[chain.count - 1];
    uint8_t *status = NULL;
    if (chain.count >= 2 && (sd->flags & VRING_DESC_F_WRITE) && sd->len >= 1)
        status = virtio_mmio_gpa(dev, sd->addr + sd->len - 1, 1);
    /* 第二个条件挡住 addr + len - 1 回绕到低地址、恰好又落在 RAM 里的情况。 */
    if (!status || sd->addr + sd->len - 1 < sd->addr) {
        blk_error(blk, "request at head %u has no writable status byte", head);
        return 0;
    }

    /*
     * 请求头：只读、至少 16 字节、在客户机 RAM 里；拷贝一份再用。
     * 拷贝到栈上同样是为了防 double fetch：hdr.type 和 hdr.sector 在检查和使用
     * 之间不会再被客户机改掉。
     */
    const struct vring_desc *hd = &chain.desc[0];
    const void *hdr_ptr = virtio_mmio_gpa(dev, hd->addr, sizeof(struct virtio_blk_req_header));
    if ((hd->flags & VRING_DESC_F_WRITE) || hd->len < sizeof(struct virtio_blk_req_header) ||
        !hdr_ptr) {
        blk_error(blk, "bad request header descriptor at head %u", head);
        *status = VIRTIO_BLK_S_IOERR;
        return 1;
    }
    struct virtio_blk_req_header hdr;
    memcpy(&hdr, hdr_ptr, sizeof(hdr));

    uint64_t written = 0;
    uint8_t result;
    switch (hdr.type) {
    case VIRTIO_BLK_T_IN:
    case VIRTIO_BLK_T_OUT:
        result = blk_rw(blk, &chain, hdr.type, hdr.sector, &written);
        break;
    case VIRTIO_BLK_T_FLUSH:
        /*
         * 客户机的 flush 意味着“之前完成的写必须持久化”，对应宿主的 fdatasync。
         * pwrite 返回只代表数据进了宿主的页缓存，宿主断电仍会丢；ext4 的日志
         * 提交依赖 flush 来保证顺序，所以这一步不能省。
         * 只有提供了 VIRTIO_BLK_F_FLUSH，Linux 才会发 flush（否则它认为设备
         * 没有写缓存，写完成就等于已落盘）。
         */
        blk->stats.flushes++;
        if (fdatasync(blk->fd) < 0) {
            blk_error(blk, "flush failed: %s", strerror(errno));
            result = VIRTIO_BLK_S_IOERR;
        } else {
            result = VIRTIO_BLK_S_OK;
        }
        break;
    case VIRTIO_BLK_T_GET_ID:
        result = blk_get_id(blk, &chain, &written);
        break;
    default:
        blk_error(blk, "unsupported request type %u", hdr.type);
        result = VIRTIO_BLK_S_UNSUPP;
        break;
    }
    *status = result;
    /*
     * used.len = device 写进客户机可写 buffer 的字节数：读到的数据 + 状态字节。
     * Linux 的 virtio_blk 并不依赖它（看状态字节），但规范要求如实填写，
     * 别的 driver 可能用它判断收到了多少数据。
     * 数据段总长受 qsz * UINT32_MAX 限制，但 used.len 只有 32 位。
     */
    return written + 1 > UINT32_MAX ? UINT32_MAX : (uint32_t)(written + 1);
}

/*
 * backend 的 process_queue 回调，每次 kick 调用一次。循环取 avail ring，直到
 * device 已经看到 driver 发布的所有链（avail.idx == last_seen_avail）。
 * 每取走一条就立即完成一条，所以这个设备永远没有“在途”的请求。
 */
static int blk_process_queue(struct virtio_mmio *dev, struct virtq *vq)
{
    struct virtio_blk *blk = dev->opaque;
    for (;;) {
        uint16_t head;
        /* 返回 1 取到一个链头，0 已经空了，-1 avail.idx 不可信。 */
        int rc = virtq_avail_consume(vq, &head);
        if (rc == 0)
            return 0;
        /*
         * avail.idx 一次前进超过 qsz，或链头下标越界：前者说明 avail ring 已经
         * 不可信，后者没法放进 used ring（id 必须是合法下标）。两种情况都无法
         * 以“完成一条请求”的方式恢复，只能让 driver 复位设备。
         */
        if (rc < 0 || head >= vq->qsz) {
            blk_error(blk, rc < 0 ? "avail.idx moved more than the queue size"
                                  : "avail ring head %u out of range", head);
            return -1;
        }
        /*
         * 不管请求成功、失败还是链非法，都要放进 used ring：driver 在等这个链头
         * 回来才能释放描述符、结束这个 I/O，不完成它就会永远卡住。
         */
        uint32_t len = blk_handle_request(blk, vq, head);
        if (virtq_used_push(vq, head, len) < 0)
            return -1;
    }
}

int virtio_blk_open(struct virtio_blk *blk, const char *path)
{
    memset(blk, 0, sizeof(*blk));
    blk->fd = open(path, O_RDWR | O_CLOEXEC);
    if (blk->fd < 0) {
        fprintf(stderr, "ERROR: open disk image %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(blk->fd, &st) < 0) {
        fprintf(stderr, "ERROR: stat disk image %s: %s\n", path, strerror(errno));
        goto fail;
    }
    /*
     * 磁盘大小：普通文件看 st_size；块设备（例如 /dev/loop0、一个 LVM 卷）的
     * st_size 是 0，要用 BLKGETSIZE64 问内核。
     */
    uint64_t bytes;
    if (S_ISREG(st.st_mode)) {
        bytes = (uint64_t)st.st_size;
    } else if (S_ISBLK(st.st_mode)) {
        if (ioctl(blk->fd, BLKGETSIZE64, &bytes) < 0) {
            fprintf(stderr, "ERROR: size of block device %s: %s\n", path, strerror(errno));
            goto fail;
        }
    } else {
        fprintf(stderr, "ERROR: disk image %s is not a regular file or block device\n", path);
        goto fail;
    }
    /*
     * 两个 VM 同时挂载同一个 ext4 镜像会互相覆盖元数据，直接损坏文件系统。
     * flock 是建议锁，只能挡住同样加锁的进程（另一个 svmm），挡不住普通的写。
     */
    if (flock(blk->fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK)
            fprintf(stderr, "ERROR: disk image %s is in use by another process\n", path);
        else
            fprintf(stderr, "ERROR: lock disk image %s: %s\n", path, strerror(errno));
        goto fail;
    }
    uint64_t capacity = bytes / VIRTIO_BLK_SECTOR_SIZE;
    if (capacity == 0) {
        fprintf(stderr, "ERROR: disk image %s is smaller than one %u-byte sector\n", path,
                VIRTIO_BLK_SECTOR_SIZE);
        goto fail;
    }
    blk->disk_bytes = capacity * VIRTIO_BLK_SECTOR_SIZE;
    /* 配置空间：driver 在 FEATURES_OK 之后从 0x100 起读这个结构。 */
    blk->config = (struct virtio_blk_config){
        .capacity = capacity,
        /* 没有 INDIRECT 时，一个请求的 头 + 数据段 + 状态 都要放进同一个队列。 */
        .seg_max = VIRTIO_BLK_QUEUE_SIZE - 2,
        .blk_size = VIRTIO_BLK_SECTOR_SIZE,
    };
    /*
     * 交给传输层的描述：设备类型、提供的特性、队列大小、配置空间、请求处理函数。
     * 特性含义：
     *   VERSION_1  modern virtio，Linux 的 virtio-mmio v2 driver 要求有
     *   SEG_MAX    告诉 driver 一个请求最多几个数据段（config.seg_max）
     *   BLK_SIZE   逻辑块大小（config.blk_size），Linux 据此设置 logical_block_size
     *   FLUSH      设备有写缓存，driver 需要发 flush 才能保证落盘
     * 没提供的（例如 RO 只读盘、DISCARD、多队列 MQ）driver 就不会使用。
     */
    blk->backend = (struct virtio_mmio_backend){
        .device_id = VIRTIO_ID_BLOCK_DEVICE,
        .features = (1ull << VIRTIO_F_VERSION_1) | (1ull << VIRTIO_BLK_F_SEG_MAX) |
                    (1ull << VIRTIO_BLK_F_BLK_SIZE) | (1ull << VIRTIO_BLK_F_FLUSH),
        .queue_num_max = VIRTIO_BLK_QUEUE_SIZE,
        .config = &blk->config,
        .config_size = sizeof(blk->config),
        .process_queue = blk_process_queue,
    };
    return 0;

fail:
    close(blk->fd);
    blk->fd = -1;
    return -1;
}

/* opaque 传 blk 自己，process_queue 回调里通过 dev->opaque 取回。 */
void virtio_blk_attach(struct virtio_blk *blk, uint8_t *mem, uint64_t mem_size,
                       void (*set_irq)(void *opaque, int level), void *irq_opaque)
{
    virtio_mmio_init(&blk->mmio, &blk->backend, blk, mem, mem_size, set_irq, irq_opaque);
}

void virtio_blk_print_stats(const struct virtio_blk *blk, FILE *stream)
{
    fprintf(stream,
            "virtio-blk: requests=%lu reads=%lu writes=%lu flushes=%lu errors=%lu "
            "bytes_read=%llu bytes_written=%llu notifies=%lu interrupts=%lu\n",
            blk->stats.requests, blk->stats.reads, blk->stats.writes, blk->stats.flushes,
            blk->stats.errors, blk->stats.bytes_read, blk->stats.bytes_written,
            blk->mmio.stats.notifies, blk->mmio.stats.interrupts);
}

/* 关闭 fd 同时释放 flock，下一个 svmm 就可以使用这个镜像了。 */
void virtio_blk_close(struct virtio_blk *blk)
{
    if (blk->fd >= 0)
        close(blk->fd);
    blk->fd = -1;
}

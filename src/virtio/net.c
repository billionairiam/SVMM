#include "virtio/net.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>

/*
 * virtio-net 设备端。函数分三组：
 *
 *   TX（vCPU 线程，客户机写 QueueNotify=1 时）
 *     net_process_queue()  → tx_one()：校验链 → 跳过 12 字节头 → writev(tap)
 *
 *   RX（RX 线程）
 *     rx_thread()          poll(tap, wake_fd) 等待“有帧”或“有新 buffer/要退出”
 *     virtio_net_rx_poll() → virtio_mmio_run_queue(RX, rx_fill)：持锁检查队列还活着，
 *                            把帧写进 buffer，按需拉中断
 *
 *   生命周期
 *     virtio_net_open() / attach() / start() / stop() / close()
 *
 * 为什么 RX 需要单独的线程？帧从 tap 到达的时刻和客户机无关。客户机空闲时 vCPU
 * 执行 HLT，KVM 在内核里处理（irqchip 模式下不退出到 VMM），vCPU 线程一直阻塞在
 * KVM_RUN 里。如果只在 VM exit 之后顺便读 tap，空闲的客户机就永远收不到包——
 * 例如它在 accept() 里等连接时。RX 线程收到帧后自己写 used ring、拉中断线，
 * KVM 把中断注入客户机，客户机从 HLT 醒来处理。
 */

#define NET_ERROR_LOG_LIMIT 16u

/* 一条链的快照，见 blk.c 的同名结构。 */
struct chain {
    struct vring_desc desc[VIRTQ_MAX_SIZE];
    unsigned int count;
};

static int collect_desc(const struct vring_desc *desc, uint16_t idx, void *opaque)
{
    (void)idx;
    struct chain *chain = opaque;
    chain->desc[chain->count++] = *desc;
    return 0;
}

__attribute__((format(printf, 2, 3)))
static void net_log(struct virtio_net *net, const char *format, ...)
{
    if (net->logged_errors >= NET_ERROR_LOG_LIMIT)
        return;
    if (++net->logged_errors == NET_ERROR_LOG_LIMIT) {
        fprintf(stderr, "virtio-net: further errors are not logged\n");
        return;
    }
    va_list args;
    va_start(args, format);
    fputs("virtio-net: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

/*
 * 取链的快照并检查：每个描述符的方向都必须是 writable（RX 全部可写，TX 全部只读），
 * 每一段都完整落在客户机 RAM 里。返回整条链的总字节数，不合法时返回 -1。
 */
static int64_t snapshot_chain(struct virtio_net *net, const struct virtq *vq, uint16_t head,
                              bool writable, struct chain *chain, const char *dir)
{
    chain->count = 0;
    if (virtq_walk_chain(vq, head, collect_desc, chain) < 0) {
        net_log(net, "%s chain at head %u rejected: invalid descriptor chain", dir, head);
        return -1;
    }
    uint64_t total = 0;
    for (unsigned int i = 0; i < chain->count; i++) {
        const struct vring_desc *d = &chain->desc[i];
        if (!!(d->flags & VRING_DESC_F_WRITE) != writable) {
            net_log(net, "%s chain at head %u rejected: descriptor %u is %s", dir, head, i,
                    writable ? "read-only" : "device-writable");
            return -1;
        }
        if (!virtio_mmio_gpa(&net->mmio, d->addr, d->len)) {
            net_log(net, "%s chain at head %u rejected: buffer [0x%llx, +0x%x) is outside "
                    "guest memory", dir, head, (unsigned long long)d->addr, d->len);
            return -1;
        }
        total += d->len;
    }
    return (int64_t)total;
}

/* ---------- TX ---------- */

/*
 * 发送一条 TX 链：[virtio_net_hdr | 以太网帧]，可以任意切分到多个只读描述符里
 * （VERSION_1 隐含 ANY_LAYOUT）。Linux 通常把头和帧放在同一段（头部空间够时）
 * 或者头单独一段、帧按 skb 的分片再分几段。
 *
 * 没有协商任何 offload，driver 发来的帧已经算好了校验和、也不会超过 MTU，
 * 头的内容可以忽略：用 iovec 跳过前 12 字节，剩下的原样 writev 给 tap，
 * 不经过中间缓冲。
 */
static void tx_one(struct virtio_net *net, struct virtq *vq, uint16_t head)
{
    struct chain chain;
    int64_t total = snapshot_chain(net, vq, head, false, &chain, "TX");
    if (total < 0) {
        net->stats.bad_chains++;
        return;
    }
    uint64_t frame_len = (uint64_t)total - (total >= VIRTIO_NET_HDR_SIZE ? VIRTIO_NET_HDR_SIZE : 0);
    if ((uint64_t)total < VIRTIO_NET_HDR_SIZE + VIRTIO_NET_ETH_HLEN ||
        frame_len > VIRTIO_NET_MAX_FRAME) {
        net_log(net, "TX chain at head %u rejected: %lld bytes is not a header plus an "
                "Ethernet frame", head, (long long)total);
        net->stats.bad_chains++;
        return;
    }

    struct iovec iov[VIRTQ_MAX_SIZE];
    int iovcnt = 0;
    uint32_t skip = VIRTIO_NET_HDR_SIZE;
    for (unsigned int i = 0; i < chain.count; i++) {
        const struct vring_desc *d = &chain.desc[i];
        if (d->len <= skip) {
            skip -= d->len; /* 这一段整个都是头（或头的一部分） */
            continue;
        }
        iov[iovcnt].iov_base = virtio_mmio_gpa(&net->mmio, d->addr + skip, 0);
        iov[iovcnt].iov_len = d->len - skip;
        iovcnt++;
        skip = 0;
    }

    ssize_t n;
    do {
        n = writev(net->tap_fd, iov, iovcnt);
    } while (n < 0 && errno == EINTR);
    /*
     * 写失败（tap 接口 down 了、对端没了、宿主机内核队列满了返回 EAGAIN）就丢掉这个帧，
     * 和真实网卡在线路忙时丢包一样；TCP 会重传。链照常完成，driver 才能回收 buffer。
     */
    if (n != (ssize_t)frame_len) {
        net->stats.tx_errors++;
        net_log(net, "TX write to tap failed: %s",
                n < 0 ? strerror(errno) : "short write");
        return;
    }
    net->stats.tx_packets++;
    net->stats.tx_bytes += frame_len;
}

/* 写一个 eventfd，唤醒 RX 线程。计数溢出（EAGAIN）也没关系：线程已经会醒。 */
static void wake_rx(struct virtio_net *net)
{
    uint64_t one = 1;
    if (net->wake_fd >= 0 && write(net->wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
        net_log(net, "wake RX thread: %s", strerror(errno));
}

/*
 * backend 的 kick 回调（vCPU 线程，持锁）。
 *   TX 队列：driver 放进来了要发的帧，同步发完。
 *   RX 队列：driver 补充了空 buffer。如果 RX 线程因为没有 buffer 停在那里，
 *            唤醒它；真正的写入在 RX 线程里做，vCPU 尽快回到客户机。
 */
static int net_process_queue(struct virtio_mmio *dev, uint32_t queue, struct virtq *vq)
{
    struct virtio_net *net = dev->opaque;
    if (queue == VIRTIO_NET_RXQ) {
        wake_rx(net);
        return 0;
    }
    for (;;) {
        uint16_t head;
        int rc = virtq_avail_consume(vq, &head);
        if (rc == 0)
            return 0;
        if (rc < 0 || head >= vq->qsz) {
            net_log(net, rc < 0 ? "TX avail.idx moved more than the queue size"
                                : "TX avail ring head %u out of range", head);
            return -1;
        }
        tx_one(net, vq, head);
        /* TX 方向 device 不往 buffer 里写任何东西，used.len = 0。 */
        if (virtq_used_push(vq, head, 0) < 0)
            return -1;
    }
}

static void net_driver_ok(struct virtio_mmio *dev)
{
    wake_rx(dev->opaque);
}

/* ---------- RX ---------- */

/* 把 data 写到链的第 offset 字节处，可能跨越多个描述符。调用方保证放得下。 */
static void scatter(struct virtio_net *net, const struct chain *chain, uint64_t offset,
                    const uint8_t *data, uint64_t len)
{
    for (unsigned int i = 0; i < chain->count && len; i++) {
        const struct vring_desc *d = &chain->desc[i];
        if (offset >= d->len) {
            offset -= d->len;
            continue;
        }
        uint64_t n = d->len - offset;
        if (n > len)
            n = len;
        memcpy(virtio_mmio_gpa(&net->mmio, d->addr + offset, 0), data, n);
        data += n;
        len -= n;
        offset = 0;
    }
}

/*
 * 确保 rx_frame 里有一个待投递的帧。tap 是 O_NONBLOCK 的，没有帧时返回 0。
 * 每次 read 正好读出一个完整的以太网帧（不含 4 字节 packet info，因为 IFF_NO_PI）。
 */
static int rx_have_frame(struct virtio_net *net)
{
    if (net->rx_frame_len)
        return 1;
    if (net->tap_broken)
        return 0;
    ssize_t n;
    do {
        n = read(net->tap_fd, net->rx_frame, sizeof(net->rx_frame));
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (n <= 0) {
        /* tap 被删掉了之类：不再 poll 它，避免 RX 线程空转。 */
        net_log(net, "read from tap failed: %s; receive path disabled",
                n < 0 ? strerror(errno) : "end of file");
        net->tap_broken = 1;
        return 0;
    }
    net->rx_frame_len = (uint32_t)n;
    return 1;
}

/*
 * virtio_mmio_run_queue() 的回调（RX 线程或测试，持锁，RX 队列活着）。
 * 只要“有空 buffer”且“有帧”就投递一个，直到两者之一用完。
 *
 * 顺序很重要：先 peek 一个 buffer，确认有地方放，才从 tap 读帧。没有 buffer 时
 * 帧留在宿主机内核的 tap 队列里（满了由内核丢弃），不会被读出来再扔掉。
 */
static int rx_fill(struct virtio_mmio *dev, struct virtq *vq, void *arg)
{
    struct virtio_net *net = arg;
    (void)dev;
    for (;;) {
        uint16_t head;
        int rc = virtq_avail_peek(vq, &head);
        if (rc < 0 || (rc == 1 && head >= vq->qsz)) {
            net_log(net, rc < 0 ? "RX avail.idx moved more than the queue size"
                                : "RX avail ring head %u out of range", head);
            return -1;
        }
        if (rc == 0) {
            net->rx_starved = 1;
            return 0;
        }
        if (!rx_have_frame(net))
            return 0;

        struct chain chain;
        int64_t capacity = snapshot_chain(net, vq, head, true, &chain, "RX");
        /*
         * 之后只用 peek 到的 head 和这份快照；consume 只是把 last_seen_avail 往前推，
         * 它读到的 head 丢掉不用（客户机可能已经改了 avail ring，不能再读一次）。
         */
        uint16_t ignored;
        if (capacity < 0) {
            /* 坏 buffer：以 len 0 还给 driver，帧留给下一个 buffer。 */
            net->stats.bad_chains++;
            virtq_avail_consume(vq, &ignored);
            if (virtq_used_push(vq, head, 0) < 0)
                return -1;
            continue;
        }
        uint64_t need = (uint64_t)VIRTIO_NET_HDR_SIZE + net->rx_frame_len;
        if ((uint64_t)capacity < need) {
            /*
             * 帧比 driver 的 buffer 大（例如宿主机把 tap 的 MTU 调到 9000，而客户机按
             * 1500 准备 buffer）。没有 MRG_RXBUF 就不能拆到多个 buffer 里，只能丢帧；
             * buffer 不动，留给下一个帧。
             */
            net->stats.rx_dropped++;
            net_log(net, "RX frame of %u bytes dropped: guest buffer holds %lld",
                    net->rx_frame_len, (long long)capacity - VIRTIO_NET_HDR_SIZE);
            net->rx_frame_len = 0;
            continue;
        }
        /*
         * 头全 0：flags=0 表示校验和已经是完整的（宿主机内核在交给不带 offload 的 tap
         * 之前已经算好了），gso_type=NONE。VERSION_1 下 num_buffers 必须是 1
         * （没有 MRG_RXBUF，一个帧只用一个 buffer）。
         */
        struct virtio_net_hdr hdr = { .num_buffers = 1 };
        scatter(net, &chain, 0, (const uint8_t *)&hdr, VIRTIO_NET_HDR_SIZE);
        scatter(net, &chain, VIRTIO_NET_HDR_SIZE, net->rx_frame, net->rx_frame_len);
        virtq_avail_consume(vq, &ignored);
        if (virtq_used_push(vq, head, (uint32_t)need) < 0)
            return -1;
        net->stats.rx_packets++;
        net->stats.rx_bytes += net->rx_frame_len;
        net->rx_frame_len = 0;
    }
}

int virtio_net_rx_poll(struct virtio_net *net)
{
    net->rx_starved = 0;
    int live = virtio_mmio_run_queue(&net->mmio, VIRTIO_NET_RXQ, rx_fill, net);
    return live && !net->rx_starved && !net->tap_broken;
}

/*
 * RX 线程主循环。每轮先尽量投递，再决定等什么：
 *   - RX 队列可用且有空 buffer：等 tap 可读或 wake_fd；
 *   - 否则（没 buffer、driver 还没准备好、正在复位）：只等 wake_fd，
 *     driver 补 buffer 的 kick 或 DRIVER_OK 会写它。
 * 不用超时轮询，空闲时线程完全睡眠。
 */
static void *rx_thread(void *arg)
{
    struct virtio_net *net = arg;
    while (!__atomic_load_n(&net->rx_stop, __ATOMIC_ACQUIRE)) {
        int want_tap = virtio_net_rx_poll(net);
        struct pollfd fds[2] = {
            { .fd = net->wake_fd, .events = POLLIN },
            { .fd = net->tap_fd, .events = POLLIN },
        };
        if (poll(fds, want_tap ? 2 : 1, -1) < 0 && errno != EINTR) {
            perror("virtio-net: poll");
            break;
        }
        if (fds[0].revents & POLLIN) {
            uint64_t count;
            if (read(net->wake_fd, &count, sizeof(count)) < 0 && errno != EAGAIN)
                perror("virtio-net: read eventfd");
        }
    }
    return NULL;
}

int virtio_net_start(struct virtio_net *net)
{
    net->rx_stop = 0;
    int rc = pthread_create(&net->rx_thread, NULL, rx_thread, net);
    if (rc) {
        fprintf(stderr, "ERROR: start virtio-net RX thread: %s\n", strerror(rc));
        return -1;
    }
    net->rx_thread_running = 1;
    return 0;
}

void virtio_net_stop(struct virtio_net *net)
{
    if (!net->rx_thread_running)
        return;
    __atomic_store_n(&net->rx_stop, 1, __ATOMIC_RELEASE);
    uint64_t one = 1;
    if (write(net->wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
        perror("virtio-net: wake RX thread");
    pthread_join(net->rx_thread, NULL);
    net->rx_thread_running = 0;
}

/* ---------- 生命周期 ---------- */

static int net_init(struct virtio_net *net, const uint8_t mac[6])
{
    static const uint8_t default_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    memset(net, 0, sizeof(*net));
    net->tap_fd = -1;
    /* RX kick 和停止请求都通过它唤醒 RX 线程；非阻塞，计数满了写会返回 EAGAIN。 */
    net->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (net->wake_fd < 0) {
        perror("ERROR: eventfd");
        return -1;
    }
    /* 52:54:00 是 QEMU/KVM 常用的本地管理前缀（locally administered，不会和真实网卡冲突）。 */
    memcpy(net->config.mac, mac ? mac : default_mac, 6);
    /*
     * 交给传输层的描述：
     *   VERSION_1  modern virtio；它也决定了 virtio_net_hdr 是 12 字节
     *   MAC        driver 用 config.mac 作为 eth0 的地址
     * 不提供 CSUM/GUEST_CSUM/GSO/MRG_RXBUF：driver 发来的帧都是算好校验和、不超过
     * MTU 的完整帧，收到的帧也必须这样，正好是不带 IFF_VNET_HDR 的 tap 的语义。
     */
    net->backend = (struct virtio_mmio_backend){
        .device_id = VIRTIO_ID_NET,
        .features = (1ull << VIRTIO_F_VERSION_1) | (1ull << VIRTIO_NET_F_MAC),
        .num_queues = 2,
        .queue_num_max = VIRTIO_NET_QUEUE_SIZE,
        .config = &net->config,
        .config_size = sizeof(net->config),
        .process_queue = net_process_queue,
        .driver_ok = net_driver_ok,
    };
    return 0;
}

int virtio_net_open(struct virtio_net *net, const char *ifname)
{
    if (net_init(net, NULL) < 0)
        return -1;
    if (!ifname || !*ifname) {
        fprintf(stderr, "ERROR: tap name is empty\n");
        goto fail;
    }
    if (strlen(ifname) >= IFNAMSIZ) {
        fprintf(stderr, "ERROR: tap name '%s' is too long (at most %d characters)\n", ifname,
                IFNAMSIZ - 1);
        goto fail;
    }
    /*
     * TUNSETIFF 对不存在的名字会“顺手”创建一个新的 tap：没有 IP、没有 up，
     * 退出后自动消失。用户打错名字时，VM 能启动但网络不通，很难排查。
     * 所以先确认接口存在（由 scripts/setup-tap.sh 或 ip tuntap add 创建）。
     */
    if (if_nametoindex(ifname) == 0) {
        fprintf(stderr, "ERROR: tap %s does not exist; create it first, e.g. "
                "sudo scripts/setup-tap.sh\n", ifname);
        goto fail;
    }
    snprintf(net->ifname, sizeof(net->ifname), "%s", ifname);
    net->tap_fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (net->tap_fd < 0) {
        fprintf(stderr, "ERROR: open /dev/net/tun: %s (is the tun module loaded?)\n",
                strerror(errno));
        goto fail;
    }
    /*
     * IFF_TAP：二层，读写完整的以太网帧（IFF_TUN 是三层，只有 IP 包）。
     * IFF_NO_PI：帧前面不加 4 字节的 struct tun_pi。
     * 不加 IFF_VNET_HDR：宿主机内核不做 offload，帧的校验和都是算好的，
     * 和我们给客户机的特性（无 offload）一致。
     */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    memcpy(ifr.ifr_name, net->ifname, sizeof(ifr.ifr_name));
    if (ioctl(net->tap_fd, TUNSETIFF, &ifr) < 0) {
        int err = errno;
        if (err == EBUSY)
            fprintf(stderr, "ERROR: tap %s is busy (already attached to another process)\n",
                    ifname);
        else if (err == EINVAL)
            fprintf(stderr, "ERROR: %s is not a single-queue TAP device\n", ifname);
        else if (err == EPERM)
            fprintf(stderr, "ERROR: attach tap %s: permission denied (need CAP_NET_ADMIN, "
                    "or create it with 'ip tuntap add ... user $USER')\n", ifname);
        else
            fprintf(stderr, "ERROR: attach tap %s: %s\n", ifname, strerror(err));
        goto fail;
    }
    return 0;

fail:
    virtio_net_close(net);
    return -1;
}

int virtio_net_open_fd(struct virtio_net *net, int fd, const uint8_t mac[6])
{
    if (net_init(net, mac) < 0) {
        close(fd);
        return -1;
    }
    net->tap_fd = fd;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("ERROR: set O_NONBLOCK");
        virtio_net_close(net);
        return -1;
    }
    snprintf(net->ifname, sizeof(net->ifname), "fd%d", fd);
    return 0;
}

/* opaque 传 net 自己，回调里通过 dev->opaque 取回。 */
void virtio_net_attach(struct virtio_net *net, uint8_t *mem, uint64_t mem_size,
                       void (*set_irq)(void *opaque, int level), void *irq_opaque)
{
    virtio_mmio_init(&net->mmio, &net->backend, net, mem, mem_size, set_irq, irq_opaque);
}

void virtio_net_print_stats(const struct virtio_net *net, FILE *stream)
{
    fprintf(stream,
            "virtio-net: tx_packets=%lu tx_bytes=%llu tx_errors=%lu rx_packets=%lu "
            "rx_bytes=%llu rx_dropped=%lu bad_chains=%lu notifies=%lu interrupts=%lu\n",
            net->stats.tx_packets, net->stats.tx_bytes, net->stats.tx_errors,
            net->stats.rx_packets, net->stats.rx_bytes, net->stats.rx_dropped,
            net->stats.bad_chains, net->mmio.stats.notifies, net->mmio.stats.interrupts);
}

/* 先停 RX 线程，再关 fd；关闭 tap fd 后接口回到“没人连接”的状态，可以再次使用。 */
void virtio_net_close(struct virtio_net *net)
{
    virtio_net_stop(net);
    if (net->tap_fd >= 0)
        close(net->tap_fd);
    if (net->wake_fd >= 0)
        close(net->wake_fd);
    net->tap_fd = -1;
    net->wake_fd = -1;
}

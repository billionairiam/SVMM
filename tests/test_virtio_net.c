/*
 * Stage 07：virtio-mmio 多队列 + virtio-net 设备的失败方式测试。
 *
 * 测试扮演客户机里的 driver（可能是恶意的），只通过 MMIO 寄存器和一块假的
 * 客户机内存跟设备交互。TAP 用一对 SOCK_SEQPACKET socket 代替：和 tap fd 一样，
 * 一次 read/write 正好是一个帧。先列出可能失败的方式：
 *
 *   传输层（从单队列改成多队列）
 *   1. QueueSel 没区分队列：配置队列 1 改掉了队列 0；不存在的队列 QueueNumMax 不是 0、
 *      也能 QueueReady=1。
 *   2. kick 不存在的队列号（2、0xffff）导致越界或被当成某个真实队列处理。
 *   3. 复位（Status=0）只清了队列 0，队列 1 仍指向旧 ring。
 *
 *   设备身份
 *   4. DeviceID 不是 1、没有 VERSION_1 或 MAC、提供了实现不了的 offload
 *      （校验和、GSO、MRG_RXBUF）；配置空间里的 MAC 按字节读出来不对。
 *
 *   TX（客户机 → tap）
 *   5. virtio_net_hdr 没剥或剥错：头单独一段、头和数据同一段、头被拆在两段里，
 *      写进 tap 的帧都必须正好是数据部分；多段数据拼接顺序错。
 *   6. 非法链：含可写描述符、总长不足 12 字节头、帧短于以太网头、GPA 在 RAM 外或
 *      回绕、描述符成环。不能写 tap、不能读宿主内存，要以 len 0 完成（driver 才能
 *      回收 buffer），计入 bad_chains，之后照常工作。
 *   7. 写 tap 失败（对端没了）时 vCPU 卡住或链没完成；应当丢包、计数、完成。
 *   8. TX 的 used.len 不是 0（device 没往 TX buffer 里写任何东西）。
 *
 *   RX（tap → 客户机）
 *   9. 头写错：必须是全 0 且 num_buffers = 1；used.len 不等于 12 + 帧长；
 *      帧跨多个可写描述符、头被拆开时数据错位。
 *  10. 没有空 buffer 时帧丢失；补 buffer 之后帧乱序。
 *  11. 帧比 buffer 大：写越界、截断后交给客户机、或者把 buffer 消耗掉；
 *      应当丢帧、计入 rx_dropped，buffer 留给下一个帧。
 *  12. 非法 RX 链（只读描述符、GPA 越界）：写了不该写的内存，或者帧跟着丢了；
 *      应当以 len 0 完成这条链，帧放进下一个合法 buffer。
 *  13. 中断：投递后不拉中断线；无视 VRING_AVAIL_F_NO_INTERRUPT；没投递也拉。
 *  14. 复位后或 DRIVER_OK 之前仍往旧 ring 里写帧。
 *  15. avail.idx 一次前进超过 qsz（TX 和 RX 都要查）：应置 DEVICE_NEEDS_RESET，
 *      复位后恢复。
 *  16. RX 线程：客户机不 kick、不产生任何 MMIO 访问时帧也要送达并拉中断；
 *      RX 队列空了之后，driver 补 buffer 并 kick 能把线程唤醒；
 *      stop 在线程阻塞在 poll() 里时也能及时返回。
 *
 *   打开 TAP
 *  17. 接口不存在时仍然成功（TUNSETIFF 会顺手创建一个新 tap）；名字太长时被截断。
 */
#include "virtio/mmio.h"
#include "virtio/net.h"
#include "virtio/virtq.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, \
                    __func__, #cond);                                            \
            failures++;                                                          \
        }                                                                        \
    } while (0)

/* 假客户机内存：两个队列的 ring + buffer 区。 */
#define RAM_SIZE (1u << 20)
#define QSZ 16u
#define BUF_GPA 0x20000u
#define BUF_STRIDE 0x1000u /* 每个描述符一块 buffer：BUF_GPA + i * BUF_STRIDE */

static const uint64_t ring_gpa[2][3] = {
    { 0x1000, 0x2000, 0x3000 }, /* RX：desc, avail, used */
    { 0x4000, 0x5000, 0x6000 }, /* TX */
};

static uint8_t *ram;
static struct virtio_net net;
static int peer = -1; /* “宿主机那一侧的 tap” */

static int irq_level;
static int irq_raises;

static void fake_set_irq(void *opaque, int level)
{
    (void)opaque;
    if (level && !__atomic_load_n(&irq_level, __ATOMIC_SEQ_CST))
        __atomic_fetch_add(&irq_raises, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&irq_level, level, __ATOMIC_SEQ_CST);
}

static uint32_t rd(uint64_t off, uint32_t len)
{
    uint8_t buf[8] = { 0 };
    uint32_t v = 0;
    virtio_mmio_access(&net.mmio, off, buf, len, 0);
    memcpy(&v, buf, len < 4 ? len : 4);
    return v;
}

static void wr(uint64_t off, uint32_t v)
{
    virtio_mmio_access(&net.mmio, off, (uint8_t *)&v, 4, 1);
}

static struct vring_desc *desc_table(int q)
{
    return (struct vring_desc *)(ram + ring_gpa[q][0]);
}

static struct vring_avail *avail_ring(int q)
{
    return (struct vring_avail *)(ram + ring_gpa[q][1]);
}

static struct vring_used *used_ring(int q)
{
    return (struct vring_used *)(ram + ring_gpa[q][2]);
}

static uint16_t used_idx(int q)
{
    return __atomic_load_n(&used_ring(q)->idx, __ATOMIC_ACQUIRE);
}

static uint64_t buf_gpa(int i)
{
    return BUF_GPA + (uint64_t)i * BUF_STRIDE;
}

static void set_desc(int q, uint16_t i, uint64_t addr, uint32_t len, uint16_t flags,
                     uint16_t next)
{
    desc_table(q)[i] = (struct vring_desc){ .addr = addr, .len = len, .flags = flags, .next = next };
}

static void publish(int q, uint16_t head)
{
    struct vring_avail *a = avail_ring(q);
    a->ring[a->idx % QSZ] = head;
    __atomic_store_n(&a->idx, (uint16_t)(a->idx + 1), __ATOMIC_RELEASE);
}

static void kick(uint32_t q)
{
    wr(VIRTIO_MMIO_QUEUE_NOTIFY, q);
}

static void write_driver_features(uint64_t f)
{
    wr(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    wr(VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)f);
    wr(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    wr(VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)(f >> 32));
}

static uint64_t device_features(void)
{
    wr(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint64_t lo = rd(VIRTIO_MMIO_DEVICE_FEATURES, 4);
    wr(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    return (uint64_t)rd(VIRTIO_MMIO_DEVICE_FEATURES, 4) << 32 | lo;
}

static void set_queue(uint32_t q, uint32_t num, const uint64_t gpa[3])
{
    wr(VIRTIO_MMIO_QUEUE_SEL, q);
    wr(VIRTIO_MMIO_QUEUE_NUM, num);
    wr(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)gpa[0]);
    wr(VIRTIO_MMIO_QUEUE_DESC_HIGH, 0);
    wr(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)gpa[1]);
    wr(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, 0);
    wr(VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)gpa[2]);
    wr(VIRTIO_MMIO_QUEUE_USED_HIGH, 0);
    wr(VIRTIO_MMIO_QUEUE_READY, 1);
}

static int queue_ready(uint32_t q)
{
    wr(VIRTIO_MMIO_QUEUE_SEL, q);
    return rd(VIRTIO_MMIO_QUEUE_READY, 4) == 1;
}

static void bring_up(void)
{
    const uint32_t s = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    memset(ram, 0, RAM_SIZE);
    wr(VIRTIO_MMIO_STATUS, 0);
    wr(VIRTIO_MMIO_STATUS, s);
    write_driver_features((1ull << VIRTIO_F_VERSION_1) | (1ull << VIRTIO_NET_F_MAC));
    wr(VIRTIO_MMIO_STATUS, s | VIRTIO_STATUS_FEATURES_OK);
    CHECK(rd(VIRTIO_MMIO_STATUS, 4) & VIRTIO_STATUS_FEATURES_OK);
    set_queue(VIRTIO_NET_RXQ, QSZ, ring_gpa[0]);
    set_queue(VIRTIO_NET_TXQ, QSZ, ring_gpa[1]);
    CHECK(queue_ready(0) && queue_ready(1));
    wr(VIRTIO_MMIO_STATUS, s | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
}

static void ack_irq(void)
{
    wr(VIRTIO_MMIO_INTERRUPT_ACK, rd(VIRTIO_MMIO_INTERRUPT_STATUS, 4));
}

static void open_device(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        perror("socketpair");
        exit(1);
    }
    if (peer >= 0)
        close(peer);
    peer = sv[1];
    fcntl(peer, F_SETFL, O_NONBLOCK);
    irq_level = 0;
    irq_raises = 0;
    if (virtio_net_open_fd(&net, sv[0], NULL) < 0) {
        fprintf(stderr, "virtio_net_open_fd failed\n");
        exit(1);
    }
    virtio_net_attach(&net, ram, RAM_SIZE, fake_set_irq, NULL);
    bring_up();
}

static void close_device(void)
{
    virtio_net_close(&net);
    close(peer);
    peer = -1;
}

/* 一个以太网帧：目的 MAC、源 MAC、类型，后面是可辨认的内容。 */
static uint32_t make_frame(uint8_t *frame, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0; i < len; i++)
        frame[i] = (uint8_t)(seed + i * 13);
    return len;
}

/* 从 peer 读一个帧；没有时返回 -1。 */
static ssize_t peer_recv(uint8_t *buf, size_t cap)
{
    return recv(peer, buf, cap, MSG_DONTWAIT);
}

static void peer_send(const uint8_t *frame, size_t len)
{
    if (send(peer, frame, len, 0) != (ssize_t)len) {
        perror("peer send");
        exit(1);
    }
}

/* ---------- TX 帮助函数 ---------- */

/* 把 frame 加上 12 字节头，按 seg_lens 切成多段放进 TX 描述符 0..n-1，发布并 kick。 */
static void tx_submit(const uint8_t *frame, uint32_t frame_len, const uint32_t *seg_lens, int nseg)
{
    uint8_t packet[VIRTIO_NET_HDR_SIZE + 4096];
    memset(packet, 0xa5, VIRTIO_NET_HDR_SIZE); /* 头里的垃圾不应影响 TX */
    memcpy(packet + VIRTIO_NET_HDR_SIZE, frame, frame_len);
    uint32_t off = 0;
    for (int i = 0; i < nseg; i++) {
        memcpy(ram + buf_gpa(i), packet + off, seg_lens[i]);
        set_desc(1, (uint16_t)i, buf_gpa(i), seg_lens[i], i + 1 < nseg ? VRING_DESC_F_NEXT : 0,
                 (uint16_t)(i + 1));
        off += seg_lens[i];
    }
    CHECK(off == VIRTIO_NET_HDR_SIZE + frame_len);
    publish(1, 0);
    kick(VIRTIO_NET_TXQ);
}

/* 5 8 */
static void test_tx_strips_header_in_any_layout(void)
{
    open_device();
    uint8_t frame[600], got[2048];
    make_frame(frame, sizeof(frame), 1);

    struct {
        const char *name;
        uint32_t segs[4];
        int n;
    } layouts[] = {
        { "header alone", { 12, 600 }, 2 },
        { "header with data", { 612 }, 1 },
        { "header split", { 5, 7, 300, 300 }, 4 },
        { "header and data split", { 20, 100, 492 }, 3 },
    };
    for (unsigned i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        uint16_t before = used_idx(1);
        frame[20] = (uint8_t)i; /* 每种布局的帧不同 */
        tx_submit(frame, sizeof(frame), layouts[i].segs, layouts[i].n);
        ssize_t n = peer_recv(got, sizeof(got));
        if (n != (ssize_t)sizeof(frame) || memcmp(got, frame, sizeof(frame)) != 0) {
            fprintf(stderr, "layout '%s': got %zd bytes\n", layouts[i].name, n);
            failures++;
        }
        CHECK((uint16_t)(used_idx(1) - before) == 1);
        CHECK(used_ring(1)->ring[before % QSZ].id == 0);
        CHECK(used_ring(1)->ring[before % QSZ].len == 0);
    }
    CHECK(net.stats.tx_packets == 4);
    CHECK(net.stats.tx_bytes == 4 * sizeof(frame));
    CHECK(irq_level == 1);
    CHECK(rd(VIRTIO_MMIO_INTERRUPT_STATUS, 4) & VIRTIO_MMIO_INT_VRING);
    ack_irq();
    CHECK(irq_level == 0);
    close_device();
}

/* 6 */
static void test_tx_rejects_malformed_chains(void)
{
    open_device();
    uint8_t got[2048];
    struct {
        const char *name;
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } bad[] = {
        { "writable descriptor", BUF_GPA, 100, VRING_DESC_F_WRITE, 0 },
        { "shorter than header", BUF_GPA, 8, 0, 0 },
        { "shorter than ethernet header", BUF_GPA, 12 + 10, 0, 0 },
        { "outside RAM", RAM_SIZE - 50, 100, 0, 0 },
        { "address wraps", UINT64_MAX - 10, 100, 0, 0 },
        { "loop", BUF_GPA, 100, VRING_DESC_F_NEXT, 0 },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint16_t before = used_idx(1);
        set_desc(1, 0, bad[i].addr, bad[i].len, bad[i].flags, bad[i].next);
        publish(1, 0);
        kick(VIRTIO_NET_TXQ);
        if (peer_recv(got, sizeof(got)) >= 0) {
            fprintf(stderr, "'%s' reached the tap\n", bad[i].name);
            failures++;
        }
        CHECK((uint16_t)(used_idx(1) - before) == 1);
        CHECK(used_ring(1)->ring[before % QSZ].len == 0);
    }
    /* 头在前、可写描述符在后：读完头才发现，也不能写 tap。 */
    set_desc(1, 0, buf_gpa(0), 100, VRING_DESC_F_NEXT, 1);
    set_desc(1, 1, buf_gpa(1), 100, VRING_DESC_F_WRITE, 0);
    publish(1, 0);
    kick(VIRTIO_NET_TXQ);
    CHECK(peer_recv(got, sizeof(got)) < 0);

    CHECK(net.stats.bad_chains == 7);
    CHECK(net.stats.tx_packets == 0);
    /* 之后合法的帧照常发出。 */
    uint8_t frame[64];
    make_frame(frame, sizeof(frame), 7);
    uint32_t seg[] = { 12 + 64 };
    tx_submit(frame, sizeof(frame), seg, 1);
    CHECK(peer_recv(got, sizeof(got)) == 64 && memcmp(got, frame, 64) == 0);
    CHECK(!(rd(VIRTIO_MMIO_STATUS, 4) & VIRTIO_STATUS_DEVICE_NEEDS_RESET));
    close_device();
}

/* 7 */
static void test_tx_write_failure_still_completes(void)
{
    open_device();
    close(peer); /* tap 的另一端没了，write 返回 EPIPE */
    peer = -1;
    uint8_t frame[100];
    make_frame(frame, sizeof(frame), 3);
    uint32_t seg[] = { 112 };
    uint16_t before = used_idx(1);
    tx_submit(frame, sizeof(frame), seg, 1);
    CHECK((uint16_t)(used_idx(1) - before) == 1);
    CHECK(net.stats.tx_errors == 1);
    CHECK(net.stats.tx_packets == 0);
    CHECK(!(rd(VIRTIO_MMIO_STATUS, 4) & VIRTIO_STATUS_DEVICE_NEEDS_RESET));
    virtio_net_close(&net);
}

/* ---------- RX 帮助函数 ---------- */

/* 用描述符 first..first+n-1 组一条可写的 RX 链，长度为 lens[]，发布（不 kick）。 */
static void rx_post(uint16_t first, const uint32_t *lens, int n)
{
    for (int i = 0; i < n; i++) {
        uint16_t d = (uint16_t)(first + i);
        memset(ram + buf_gpa(d), 0xcc, lens[i]);
        set_desc(0, d, buf_gpa(d), lens[i],
                 VRING_DESC_F_WRITE | (i + 1 < n ? VRING_DESC_F_NEXT : 0), (uint16_t)(d + 1));
    }
    publish(0, first);
}

/* 把 RX 链 first..（长度 lens）里的内容按顺序拼起来。 */
static uint32_t rx_gather(uint16_t first, const uint32_t *lens, int n, uint8_t *out)
{
    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        memcpy(out + off, ram + buf_gpa(first + i), lens[i]);
        off += lens[i];
    }
    return off;
}

static int hdr_ok(const uint8_t *p)
{
    struct virtio_net_hdr h;
    memcpy(&h, p, sizeof(h));
    return h.flags == 0 && h.gso_type == 0 && h.hdr_len == 0 && h.gso_size == 0 &&
           h.csum_start == 0 && h.csum_offset == 0 && h.num_buffers == 1;
}

/* 9 13 */
static void test_rx_writes_header_and_frame(void)
{
    open_device();
    struct {
        const char *name;
        uint32_t lens[4];
        int n;
    } layouts[] = {
        { "one buffer (Linux small buffers)", { 1600 }, 1 },
        { "header alone", { 12, 2000 }, 2 },
        { "header split", { 5, 7, 40, 2000 }, 4 },
    };
    uint8_t frame[1000], got[8192];
    for (unsigned i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        make_frame(frame, sizeof(frame), (uint8_t)(i + 9));
        uint16_t before = used_idx(0);
        rx_post(0, layouts[i].lens, layouts[i].n);
        peer_send(frame, sizeof(frame));
        virtio_net_rx_poll(&net);
        CHECK((uint16_t)(used_idx(0) - before) == 1);
        struct vring_used_elem e = used_ring(0)->ring[before % QSZ];
        CHECK(e.id == 0);
        CHECK(e.len == VIRTIO_NET_HDR_SIZE + sizeof(frame));
        rx_gather(0, layouts[i].lens, layouts[i].n, got);
        if (!hdr_ok(got) || memcmp(got + VIRTIO_NET_HDR_SIZE, frame, sizeof(frame)) != 0) {
            fprintf(stderr, "layout '%s': wrong contents\n", layouts[i].name);
            failures++;
        }
        CHECK(irq_level == 1);
        ack_irq();
    }
    CHECK(net.stats.rx_packets == 3);
    CHECK(net.stats.rx_bytes == 3 * sizeof(frame));

    /* 没有帧时不拉中断。 */
    uint32_t len = 1600;
    rx_post(0, &len, 1);
    int raises = irq_raises;
    virtio_net_rx_poll(&net);
    CHECK(irq_raises == raises && irq_level == 0);

    /* driver 关了中断（NAPI 轮询中）：只更新 ring，不拉线。 */
    avail_ring(0)->flags = VRING_AVAIL_F_NO_INTERRUPT;
    peer_send(frame, sizeof(frame));
    uint16_t before = used_idx(0);
    virtio_net_rx_poll(&net);
    CHECK((uint16_t)(used_idx(0) - before) == 1);
    CHECK(irq_raises == raises && irq_level == 0);
    close_device();
}

/* 10 */
static void test_rx_waits_for_buffers_and_keeps_order(void)
{
    open_device();
    uint8_t a[200], b[300], got[4096];
    make_frame(a, sizeof(a), 0x11);
    make_frame(b, sizeof(b), 0x22);
    peer_send(a, sizeof(a));
    peer_send(b, sizeof(b));
    /* 还没有 buffer：什么都不做，也不能丢。 */
    CHECK(virtio_net_rx_poll(&net) == 0);
    CHECK(used_idx(0) == 0);
    CHECK(net.stats.rx_dropped == 0);

    uint32_t len = 1600;
    rx_post(0, &len, 1);
    virtio_net_rx_poll(&net);
    CHECK(used_idx(0) == 1);
    rx_gather(0, &len, 1, got);
    CHECK(used_ring(0)->ring[0].len == 12 + sizeof(a) && memcmp(got + 12, a, sizeof(a)) == 0);

    rx_post(1, &len, 1);
    virtio_net_rx_poll(&net);
    CHECK(used_idx(0) == 2);
    rx_gather(1, &len, 1, got);
    CHECK(used_ring(0)->ring[1].id == 1);
    CHECK(used_ring(0)->ring[1].len == 12 + sizeof(b) && memcmp(got + 12, b, sizeof(b)) == 0);
    CHECK(net.stats.rx_packets == 2);
    close_device();
}

/* 11 */
static void test_rx_drops_oversized_frame(void)
{
    open_device();
    uint8_t big[500], small[50], got[4096];
    make_frame(big, sizeof(big), 0x33);
    make_frame(small, sizeof(small), 0x44);
    uint32_t lens[] = { 12, 100 };
    rx_post(0, lens, 2);
    /* buffer 后面的内存：写越界时会被改掉。 */
    memset(ram + buf_gpa(1) + 100, 0x5a, 64);
    peer_send(big, sizeof(big));
    peer_send(small, sizeof(small));
    virtio_net_rx_poll(&net);
    CHECK(net.stats.rx_dropped == 1);
    CHECK(used_idx(0) == 1);
    CHECK(used_ring(0)->ring[0].len == 12 + sizeof(small));
    rx_gather(0, lens, 2, got);
    CHECK(memcmp(got + 12, small, sizeof(small)) == 0);
    for (int i = 0; i < 64; i++)
        CHECK(ram[buf_gpa(1) + 100 + i] == 0x5a);
    close_device();
}

/* 12 */
static void test_rx_rejects_malformed_chains(void)
{
    open_device();
    uint8_t frame[300], got[4096];
    make_frame(frame, sizeof(frame), 0x55);
    /* 链 0：只读描述符；链 1：GPA 在 RAM 外；链 2：合法。 */
    set_desc(0, 0, buf_gpa(0), 1600, 0, 0);
    publish(0, 0);
    set_desc(0, 1, RAM_SIZE - 100, 1600, VRING_DESC_F_WRITE, 0);
    publish(0, 1);
    memset(ram + buf_gpa(0), 0x77, 1600);
    uint32_t len = 1600;
    rx_post(2, &len, 1);
    peer_send(frame, sizeof(frame));
    virtio_net_rx_poll(&net);
    CHECK(used_idx(0) == 3);
    CHECK(used_ring(0)->ring[0].id == 0 && used_ring(0)->ring[0].len == 0);
    CHECK(used_ring(0)->ring[1].id == 1 && used_ring(0)->ring[1].len == 0);
    CHECK(used_ring(0)->ring[2].id == 2 && used_ring(0)->ring[2].len == 12 + sizeof(frame));
    rx_gather(2, &len, 1, got);
    CHECK(memcmp(got + 12, frame, sizeof(frame)) == 0);
    for (int i = 0; i < 1600; i++)
        if (ram[buf_gpa(0) + i] != 0x77) {
            failures++;
            fprintf(stderr, "read-only RX buffer was written\n");
            break;
        }
    CHECK(net.stats.bad_chains == 2);
    CHECK(net.stats.rx_packets == 1);
    close_device();
}

/* 14 */
static void test_rx_respects_reset_and_driver_ok(void)
{
    open_device();
    uint8_t frame[100];
    make_frame(frame, sizeof(frame), 0x66);
    uint32_t len = 1600;
    rx_post(0, &len, 1);
    wr(VIRTIO_MMIO_STATUS, 0); /* 复位：driver 可能已经释放了这块 ring */
    CHECK(!queue_ready(0) && !queue_ready(1));
    peer_send(frame, sizeof(frame));
    CHECK(virtio_net_rx_poll(&net) == 0);
    CHECK(used_idx(0) == 0);
    CHECK(ram[buf_gpa(0)] == 0xcc);

    /* 重新配好队列但还没 DRIVER_OK：同样不能写。 */
    const uint32_t s = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    wr(VIRTIO_MMIO_STATUS, s);
    write_driver_features((1ull << VIRTIO_F_VERSION_1) | (1ull << VIRTIO_NET_F_MAC));
    wr(VIRTIO_MMIO_STATUS, s | VIRTIO_STATUS_FEATURES_OK);
    set_queue(0, QSZ, ring_gpa[0]);
    set_queue(1, QSZ, ring_gpa[1]);
    CHECK(virtio_net_rx_poll(&net) == 0);
    CHECK(used_idx(0) == 0);

    /* DRIVER_OK 之后，复位前到达的帧照常送达。 */
    wr(VIRTIO_MMIO_STATUS, s | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    virtio_net_rx_poll(&net);
    CHECK(used_idx(0) == 1);
    close_device();
}

/* 15 */
static void test_bad_avail_idx_needs_reset(void)
{
    for (int q = 0; q < 2; q++) {
        open_device();
        __atomic_store_n(&avail_ring(q)->idx, (uint16_t)(QSZ + 1), __ATOMIC_RELEASE);
        uint8_t frame[100];
        make_frame(frame, sizeof(frame), 1);
        if (q == 0) {
            peer_send(frame, sizeof(frame));
            virtio_net_rx_poll(&net);
        } else {
            kick(VIRTIO_NET_TXQ);
        }
        CHECK(rd(VIRTIO_MMIO_STATUS, 4) & VIRTIO_STATUS_DEVICE_NEEDS_RESET);
        CHECK(rd(VIRTIO_MMIO_INTERRUPT_STATUS, 4) & VIRTIO_MMIO_INT_CONFIG);
        /* 复位并重新初始化之后恢复正常。 */
        bring_up();
        CHECK(!(rd(VIRTIO_MMIO_STATUS, 4) & VIRTIO_STATUS_DEVICE_NEEDS_RESET));
        uint32_t len = 1600;
        rx_post(0, &len, 1);
        if (q == 1)
            peer_send(frame, sizeof(frame));
        virtio_net_rx_poll(&net);
        CHECK(used_idx(0) == 1);
        close_device();
    }
}

/* 1 2 3 4 */
static void test_identity_and_queues(void)
{
    open_device();
    CHECK(rd(VIRTIO_MMIO_MAGIC_VALUE, 4) == VIRTIO_MMIO_MAGIC);
    CHECK(rd(VIRTIO_MMIO_DEVICE_ID, 4) == VIRTIO_ID_NET);
    uint64_t f = device_features();
    CHECK(f & (1ull << VIRTIO_F_VERSION_1));
    CHECK(f & (1ull << VIRTIO_NET_F_MAC));
    /* CSUM(0) GUEST_CSUM(1) GUEST_TSO4..GUEST_UFO(7-10) HOST_TSO4..HOST_UFO(11-14) MRG_RXBUF(15) */
    uint64_t offloads = 0x3ull | (0x1ffull << 7);
    CHECK((f & offloads) == 0);
    const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    for (int i = 0; i < 6; i++)
        CHECK(rd(VIRTIO_MMIO_CONFIG + i, 1) == mac[i]);

    for (uint32_t q = 0; q < 2; q++) {
        wr(VIRTIO_MMIO_QUEUE_SEL, q);
        CHECK(rd(VIRTIO_MMIO_QUEUE_NUM_MAX, 4) == VIRTIO_NET_QUEUE_SIZE);
    }
    wr(VIRTIO_MMIO_QUEUE_SEL, 2);
    CHECK(rd(VIRTIO_MMIO_QUEUE_NUM_MAX, 4) == 0);
    static const uint64_t q2[3] = { 0x7000, 0x8000, 0x9000 };
    set_queue(2, QSZ, q2);
    CHECK(!queue_ready(2));

    /* 两个队列的配置互不覆盖：bring_up() 先配 RX 再配 TX，TX 的 kick 走 TX 的 ring。 */
    CHECK(net.mmio.queues[0].vq.desc == desc_table(0));
    CHECK(net.mmio.queues[1].vq.desc == desc_table(1));
    uint8_t frame[64], got[256];
    make_frame(frame, sizeof(frame), 2);
    uint32_t seg[] = { 12 + 64 };
    tx_submit(frame, sizeof(frame), seg, 1);
    CHECK(used_idx(1) == 1 && used_idx(0) == 0);
    CHECK(peer_recv(got, sizeof(got)) == 64);

    /* 不存在的队列号：什么都不发生。 */
    unsigned long notifies = net.mmio.stats.notifies;
    publish(1, 0);
    kick(2);
    kick(0xffff);
    CHECK(net.mmio.stats.notifies == notifies);
    CHECK(used_idx(1) == 1);
    CHECK(peer_recv(got, sizeof(got)) < 0);

    /* 复位清掉所有队列。 */
    wr(VIRTIO_MMIO_STATUS, 0);
    CHECK(!queue_ready(0) && !queue_ready(1));
    CHECK(net.mmio.queues[1].vq.desc == NULL);
    close_device();
}

/* ---------- RX 线程 ---------- */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int wait_used(int q, uint16_t want, unsigned timeout_ms)
{
    uint64_t end = now_ms() + timeout_ms;
    while (used_idx(q) != want) {
        if (now_ms() > end)
            return 0;
        usleep(1000);
    }
    return 1;
}

/* 16 */
static void test_rx_thread_delivers_without_kick(void)
{
    open_device();
    CHECK(virtio_net_start(&net) == 0);
    uint32_t len = 1600;
    rx_post(0, &len, 1);
    rx_post(1, &len, 1);
    kick(VIRTIO_NET_RXQ);

    /* 帧到达时客户机什么都不做（没有 MMIO 访问），线程自己送达并拉中断。 */
    uint8_t frame[400];
    make_frame(frame, sizeof(frame), 0x77);
    peer_send(frame, sizeof(frame));
    CHECK(wait_used(0, 1, 2000));
    uint64_t end = now_ms() + 2000;
    while (!__atomic_load_n(&irq_level, __ATOMIC_SEQ_CST) && now_ms() < end)
        usleep(1000);
    CHECK(__atomic_load_n(&irq_level, __ATOMIC_SEQ_CST) == 1);
    ack_irq();

    /* 用光 buffer，再来两个帧：线程只能等；补 buffer + kick 把它唤醒。 */
    peer_send(frame, sizeof(frame));
    CHECK(wait_used(0, 2, 2000));
    peer_send(frame, sizeof(frame));
    usleep(50 * 1000);
    CHECK(used_idx(0) == 2);
    rx_post(2, &len, 1);
    kick(VIRTIO_NET_RXQ);
    CHECK(wait_used(0, 3, 2000));
    CHECK(net.stats.rx_packets == 3 && net.stats.rx_dropped == 0);

    /* 线程正阻塞在 poll() 里：stop 要及时返回。 */
    uint64_t t0 = now_ms();
    virtio_net_stop(&net);
    CHECK(now_ms() - t0 < 1000);
    close_device();
}

/* 17 */
static void test_open_missing_tap(void)
{
    struct virtio_net *n = malloc(sizeof(*n));
    CHECK(virtio_net_open(n, "svmmnosuch0") < 0);
    CHECK(if_nametoindex("svmmnosuch0") == 0);
    CHECK(virtio_net_open(n, "svmm-name-way-too-long") < 0);
    CHECK(virtio_net_open(n, "") < 0);
    virtio_net_close(n);
    free(n);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    ram = aligned_alloc(4096, RAM_SIZE);
    if (!ram)
        return 1;
    test_identity_and_queues();
    test_tx_strips_header_in_any_layout();
    test_tx_rejects_malformed_chains();
    test_tx_write_failure_still_completes();
    test_rx_writes_header_and_frame();
    test_rx_waits_for_buffers_and_keeps_order();
    test_rx_drops_oversized_frame();
    test_rx_rejects_malformed_chains();
    test_rx_respects_reset_and_driver_ok();
    test_bad_avail_idx_needs_reset();
    test_rx_thread_delivers_without_kick();
    test_open_missing_tap();
    free(ram);
    if (failures) {
        fprintf(stderr, "test_virtio_net: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_virtio_net: all tests passed\n");
    return 0;
}

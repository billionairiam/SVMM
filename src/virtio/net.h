#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

/*
 * virtio-net 设备（Virtio 1.0 §5.1），后端是宿主机上的 TAP 接口。
 *
 *   客户机 eth0 ──virtio-net──► 本设备 ──read/write──► /dev/net/tun (tap0) ──► 宿主网络栈
 *
 * TAP 是一个“一端是网卡、一端是文件描述符”的虚拟接口：宿主网络栈往 tap0 发的
 * 以太网帧，VMM 从 fd 里 read() 出来；VMM write() 进 fd 的帧，宿主网络栈当作从
 * tap0 收到的。VMM 只在两边之间搬运完整的以太网帧，不解析 IP。
 *
 * 两个队列：
 *   队列 0 receiveq（RX）：driver 预先放进来一批“空的、device 可写”的 buffer，
 *                          device 收到帧时填进去再放回 used ring。
 *   队列 1 transmitq（TX）：driver 放进来“只读”的 buffer，内容是要发的帧，
 *                          device 写进 tap 后放回 used ring。
 *
 * 每个帧前面都有一个 virtio_net_hdr（协商了 VERSION_1 时 12 字节），描述校验和/
 * GSO offload。本设备不提供任何 offload 特性，所以 TX 方向忽略头的内容，
 * RX 方向写全 0（num_buffers 写 1）。
 *
 * 线程：TX 在 vCPU 线程上、客户机 kick 时同步处理；RX 由单独的线程 poll() tap fd，
 * 有帧且 RX 队列有空 buffer 时写进去并拉中断。两个线程通过 virtio_mmio 的锁互斥。
 */

#include "virtio/mmio.h"

#include <net/if.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define VIRTIO_ID_NET 1u
#define VIRTIO_NET_QUEUE_SIZE 256u
#define VIRTIO_NET_RXQ 0u
#define VIRTIO_NET_TXQ 1u

/* 特性位（§5.1.3）。只提供 MAC：driver 从配置空间读 MAC 地址，而不是随机生成。 */
#define VIRTIO_NET_F_MAC 5u

/*
 * 帧的最大长度：tap 的 MTU 最大 65535，加以太网头和一个 VLAN 标签。
 * RX 的读缓冲按这个大小分配，保证从 tap 读出来的帧不会被截断。
 */
#define VIRTIO_NET_MAX_FRAME (65535u + 14u + 4u)
#define VIRTIO_NET_ETH_HLEN 14u

/*
 * virtio_net_hdr（§5.1.6）。VERSION_1 下总是包含 num_buffers，共 12 字节；
 * 传统接口（没有 VERSION_1 和 MRG_RXBUF）只有前 10 字节。
 */
struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};
#define VIRTIO_NET_HDR_SIZE 12u

/* 配置空间（从 MMIO 0x100 起）。只有 VIRTIO_NET_F_MAC 对应的 mac 字段。 */
struct virtio_net_config {
    uint8_t mac[6];
};

struct virtio_net_stats {
    unsigned long tx_packets;
    unsigned long long tx_bytes;
    unsigned long tx_errors;  /* 链合法但写 tap 失败（帧被丢弃） */
    unsigned long rx_packets;
    unsigned long long rx_bytes;
    unsigned long rx_dropped; /* 帧比 driver 的 buffer 大，丢弃 */
    unsigned long bad_chains; /* 链结构/方向/GPA 不合法，以 len 0 完成 */
};

struct virtio_net {
    int tap_fd;  /* O_NONBLOCK；-1 表示未打开 */
    int wake_fd; /* eventfd：RX kick 和停止请求都通过它唤醒 RX 线程 */
    char ifname[IFNAMSIZ];

    struct virtio_net_config config;
    struct virtio_mmio_backend backend;
    struct virtio_mmio mmio;

    /*
     * RX 线程的状态。rx_frame 里是一个已经从 tap 读出、还没放进客户机的帧
     * （rx_frame_len > 0）。RX 队列没有空 buffer 时帧留在这里，
     * 并且不再从 tap 读，让宿主机内核的 tap 队列来做背压。
     */
    pthread_t rx_thread;
    int rx_thread_running;
    volatile int rx_stop;
    int rx_starved; /* 上一轮因为没有 RX buffer 而停下 */
    int tap_broken; /* 读 tap 出错（接口被删等），不再从它接收 */
    uint32_t rx_frame_len;
    uint8_t rx_frame[VIRTIO_NET_MAX_FRAME];

    struct virtio_net_stats stats;
    unsigned logged_errors;
};

/*
 * 打开已存在的 TAP 接口 ifname（IFF_TAP | IFF_NO_PI）。接口不存在时失败，
 * 不会隐式创建一个没配置 IP 的新接口；已被别的进程打开时失败（EBUSY）。
 */
int virtio_net_open(struct virtio_net *net, const char *ifname);

/* 用任意“一次 read/write 一个帧”的 fd 代替 TAP（测试用 SOCK_SEQPACKET）。接管 fd。 */
int virtio_net_open_fd(struct virtio_net *net, int fd, const uint8_t mac[6]);

void virtio_net_attach(struct virtio_net *net, uint8_t *mem, uint64_t mem_size,
                       void (*set_irq)(void *opaque, int level), void *irq_opaque);

/*
 * 把 tap 里已经到达的帧尽可能放进 RX 队列（不阻塞），需要时拉中断。
 * RX 线程每次被唤醒调用一次；测试也可以不启动线程直接调用。
 * 返回 1 表示 RX 队列可用且还有空 buffer（值得继续 poll tap），0 表示不值得。
 */
int virtio_net_rx_poll(struct virtio_net *net);

int virtio_net_start(struct virtio_net *net);
void virtio_net_stop(struct virtio_net *net);

void virtio_net_print_stats(const struct virtio_net *net, FILE *stream);
void virtio_net_close(struct virtio_net *net);

#endif

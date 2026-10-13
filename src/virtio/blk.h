#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

/*
 * virtio-blk 设备（Virtio 1.0 §5.2），后端是宿主机上的一个 raw 磁盘镜像文件。
 *
 * 每个请求是一条描述符链：
 *
 *   desc[0]        请求头 16 字节，只读：type、reserved、sector（512 字节扇区号）
 *   desc[1..n-2]   数据 buffer：读盘（T_IN）时可写，写盘（T_OUT）时只读
 *   desc[n-1]      状态字节，可写：0 成功，1 I/O 错误，2 不支持
 *
 * 链里的地址、长度、扇区号全部由客户机控制，处理前逐项检查（见 blk.c 的
 * blk_handle_request()）。任何一项不合法都不做 I/O，只写 IOERR 状态并照常完成，
 * 不能让 driver 永远等一个不会到来的完成项。
 */

#include "virtio/mmio.h"

#include <stdint.h>
#include <stdio.h>

#define VIRTIO_ID_BLOCK_DEVICE 2u
#define VIRTIO_BLK_SECTOR_SIZE 512u
#define VIRTIO_BLK_QUEUE_SIZE 128u
#define VIRTIO_BLK_ID_BYTES 20u

/* 请求类型。 */
#define VIRTIO_BLK_T_IN 0u
#define VIRTIO_BLK_T_OUT 1u
#define VIRTIO_BLK_T_FLUSH 4u
#define VIRTIO_BLK_T_GET_ID 8u

/* 状态字节。 */
#define VIRTIO_BLK_S_OK 0u
#define VIRTIO_BLK_S_IOERR 1u
#define VIRTIO_BLK_S_UNSUPP 2u

/* 特性位。 */
#define VIRTIO_BLK_F_SEG_MAX 2u
#define VIRTIO_BLK_F_BLK_SIZE 6u
#define VIRTIO_BLK_F_FLUSH 9u

struct virtio_blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};
_Static_assert(sizeof(struct virtio_blk_req_header) == 16, "virtio-blk header is 16 bytes");

/*
 * 配置空间（§5.2.4）的前 24 字节，到 blk_size 为止，字段偏移由规范固定。
 * 只填我们提供的特性对应的字段，其余为 0。
 */
struct virtio_blk_config {
    uint64_t capacity; /* 0x00：512 字节扇区数 */
    uint32_t size_max; /* 0x08：VIRTIO_BLK_F_SIZE_MAX，未提供 */
    uint32_t seg_max;  /* 0x0c：VIRTIO_BLK_F_SEG_MAX，单个请求最多几个数据段 */
    uint16_t cylinders;
    uint8_t heads;
    uint8_t sectors;   /* 0x10：geometry，未提供 */
    uint32_t blk_size; /* 0x14：VIRTIO_BLK_F_BLK_SIZE */
};
_Static_assert(sizeof(struct virtio_blk_config) == 24, "virtio_blk_config layout");

struct virtio_blk_stats {
    unsigned long requests;
    unsigned long reads;
    unsigned long writes;
    unsigned long flushes;
    unsigned long errors;      /* 写了 IOERR/UNSUPP 状态或无法写状态的请求 */
    unsigned long long bytes_read;
    unsigned long long bytes_written;
};

struct virtio_blk {
    int fd;
    uint64_t disk_bytes; /* 可访问的字节数 = capacity * 512 */
    struct virtio_blk_config config;
    struct virtio_mmio_backend backend;
    struct virtio_mmio mmio;
    struct virtio_blk_stats stats;
    unsigned int logged_errors; /* 限制错误日志条数，防止客户机刷屏 */
};

/*
 * 打开磁盘镜像（必须是可读写的普通文件或块设备，至少 512 字节），并加独占锁，
 * 防止两个 VMM 同时写同一个镜像。镜像末尾不足一个扇区的部分不可见。
 * 失败时打印原因并返回 -1，blk 处于可以安全调用 virtio_blk_close() 的状态。
 */
int virtio_blk_open(struct virtio_blk *blk, const char *path);

/* 把设备接到客户机内存和中断线上，传输层进入复位状态。 */
void virtio_blk_attach(struct virtio_blk *blk, uint8_t *mem, uint64_t mem_size,
                       void (*set_irq)(void *opaque, int level), void *irq_opaque);

/* 打印一行统计（请求数、字节数、kick 和中断次数）到 stream。 */
void virtio_blk_print_stats(const struct virtio_blk *blk, FILE *stream);

void virtio_blk_close(struct virtio_blk *blk);

#endif

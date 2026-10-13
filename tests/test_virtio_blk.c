/*
 * Stage 06：virtio-mmio 传输层 + virtio-blk 设备的失败方式测试。
 *
 * 测试扮演客户机里的 driver（可能是恶意的），只通过 MMIO 寄存器和一块假的
 * 客户机内存跟设备交互，后端是一个临时磁盘镜像。先列出可能失败的方式：
 *
 *   1. 设备身份或配置空间读错：Magic/Version/DeviceID 不对，Linux 不会 probe；
 *      capacity 按 4 字节或 1 字节分段读出来的值不对。
 *   2. 特性协商：driver 选了 device 没提供的特性、或没选 VERSION_1，
 *      FEATURES_OK 仍被保留；FEATURES_OK 之后还能改特性。
 *   3. 队列配置：QueueNum 为 0/非 2 的幂/超过上限，ring 未对齐、不在客户机 RAM
 *      里或 gpa + size 回绕，仍然 QueueReady=1；DRIVER_OK 之前 kick 就开始处理。
 *   4. 正常读、写、flush、GET_ID 结果不对：读到的数据和镜像不一致，写没落盘，
 *      used.len 不等于写进客户机的字节数。
 *   5. 数据 buffer 的 GPA 在客户机 RAM 之外或 addr + len 回绕：device 读写宿主内存。
 *   6. 扇区号越过磁盘末尾、部分越过、sector * 512 溢出：读写到镜像之外。
 *   7. 方向错：T_IN 的数据 buffer 是只读的（device 会写 driver 不让写的内存），
 *      T_OUT 的数据 buffer 是可写的。
 *   8. 数据长度不是 512 的整数倍。
 *   9. 请求头描述符太短或可写、状态描述符只读/长度为 0/缺失：device 崩溃、
 *      不完成请求（driver 永远等），或之后不能再处理请求。
 *  10. 描述符成环、next 越界：同上。
 *  11. avail.idx 一次前进超过 qsz、avail ring 里的链头越界：device 把旧槽位
 *      当新请求，或在无法完成的链上卡住；应置 DEVICE_NEEDS_RESET，复位后恢复。
 *  12. 多段请求后面某段非法，但前面的段已经写进镜像（做了一半的 I/O）。
 *  13. 中断：完成后不拉高中断线/不置 InterruptStatus；ACK 之后不拉低；
 *      无视 VRING_AVAIL_F_NO_INTERRUPT；复位后中断线还是高的。
 *  14. 非法 MMIO 访问（越界、未对齐、宽度不对、写只读寄存器）导致崩溃或改变状态。
 *  15. 镜像在打开后被截短：读到 EOF 仍返回 OK（客户机拿到未初始化数据）。
 *  16. 未知请求类型没有返回 UNSUPP。
 *  17. 打开镜像：不存在、太小、已被另一个 VMM 打开时仍然成功。
 */
#include "virtio/blk.h"
#include "virtio/mmio.h"
#include "virtio/virtq.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* 假客户机内存布局。 */
#define RAM_SIZE (1u << 20)
#define DESC_GPA 0x1000u
#define AVAIL_GPA 0x2000u
#define USED_GPA 0x3000u
#define HDR_GPA 0x10000u
#define STATUS_GPA 0x11000u
#define DATA_GPA 0x20000u
#define QSZ 16u

#define DISK_SECTORS 64u
#define DISK_BYTES (DISK_SECTORS * VIRTIO_BLK_SECTOR_SIZE)

static uint8_t *ram;
static char image_path[] = "/tmp/svmm-test-blk-XXXXXX";
static uint8_t image_orig[DISK_BYTES];

static int irq_level;
static int irq_raises;

static void fake_set_irq(void *opaque, int level)
{
    (void)opaque;
    if (level && !irq_level)
        irq_raises++;
    irq_level = level;
}

static uint32_t rd(struct virtio_blk *blk, uint64_t off, uint32_t len)
{
    uint8_t buf[8] = { 0 };
    uint32_t v = 0;
    virtio_mmio_access(&blk->mmio, off, buf, len, 0);
    memcpy(&v, buf, len < 4 ? len : 4);
    return v;
}

static void wr(struct virtio_blk *blk, uint64_t off, uint32_t v)
{
    virtio_mmio_access(&blk->mmio, off, (uint8_t *)&v, 4, 1);
}

static uint32_t status_reg(struct virtio_blk *blk)
{
    return rd(blk, VIRTIO_MMIO_STATUS, 4);
}

static void make_image(void)
{
    for (uint32_t i = 0; i < DISK_BYTES; i++)
        image_orig[i] = (uint8_t)(i / VIRTIO_BLK_SECTOR_SIZE * 7 + i);
    int fd = open(image_path, O_WRONLY | O_TRUNC);
    if (fd < 0 || pwrite(fd, image_orig, DISK_BYTES, 0) != (ssize_t)DISK_BYTES) {
        perror("make_image");
        exit(1);
    }
    close(fd);
}

static int image_equals_orig(void)
{
    uint8_t buf[DISK_BYTES];
    int fd = open(image_path, O_RDONLY);
    ssize_t n = pread(fd, buf, sizeof(buf), 0);
    close(fd);
    return n == (ssize_t)DISK_BYTES && memcmp(buf, image_orig, DISK_BYTES) == 0;
}

static void open_device(struct virtio_blk *blk)
{
    make_image();
    memset(ram, 0, RAM_SIZE);
    irq_level = 0;
    irq_raises = 0;
    if (virtio_blk_open(blk, image_path) < 0) {
        fprintf(stderr, "virtio_blk_open failed\n");
        exit(1);
    }
    virtio_blk_attach(blk, ram, RAM_SIZE, fake_set_irq, NULL);
}

static uint64_t device_features(struct virtio_blk *blk)
{
    wr(blk, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint64_t lo = rd(blk, VIRTIO_MMIO_DEVICE_FEATURES, 4);
    wr(blk, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint64_t hi = rd(blk, VIRTIO_MMIO_DEVICE_FEATURES, 4);
    return hi << 32 | lo;
}

static void write_driver_features(struct virtio_blk *blk, uint64_t f)
{
    wr(blk, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    wr(blk, VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)f);
    wr(blk, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    wr(blk, VIRTIO_MMIO_DRIVER_FEATURES, (uint32_t)(f >> 32));
}

/* 走到 FEATURES_OK 为止，返回 FEATURES_OK 是否被 device 保留。 */
static int negotiate(struct virtio_blk *blk, uint64_t features)
{
    wr(blk, VIRTIO_MMIO_STATUS, 0);
    wr(blk, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    wr(blk, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    write_driver_features(blk, features);
    wr(blk, VIRTIO_MMIO_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    return (status_reg(blk) & VIRTIO_STATUS_FEATURES_OK) != 0;
}

static void set_queue(struct virtio_blk *blk, uint32_t num, uint64_t desc, uint64_t avail,
                      uint64_t used)
{
    wr(blk, VIRTIO_MMIO_QUEUE_SEL, 0);
    wr(blk, VIRTIO_MMIO_QUEUE_NUM, num);
    wr(blk, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc);
    wr(blk, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(desc >> 32));
    wr(blk, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail);
    wr(blk, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(avail >> 32));
    wr(blk, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used);
    wr(blk, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(used >> 32));
    wr(blk, VIRTIO_MMIO_QUEUE_READY, 1);
}

static int queue_ready(struct virtio_blk *blk)
{
    return rd(blk, VIRTIO_MMIO_QUEUE_READY, 4) == 1;
}

/* 完整的 Linux 式初始化。 */
static void bring_up(struct virtio_blk *blk)
{
    uint64_t want = (1ull << VIRTIO_F_VERSION_1) | (1ull << VIRTIO_BLK_F_FLUSH) |
                    (1ull << VIRTIO_BLK_F_BLK_SIZE) | (1ull << VIRTIO_BLK_F_SEG_MAX);
    CHECK(negotiate(blk, want));
    set_queue(blk, QSZ, DESC_GPA, AVAIL_GPA, USED_GPA);
    CHECK(queue_ready(blk));
    wr(blk, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                     VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
}

static struct vring_desc *desc_table(void)
{
    return (struct vring_desc *)(ram + DESC_GPA);
}

static struct vring_avail *avail_ring(void)
{
    return (struct vring_avail *)(ram + AVAIL_GPA);
}

static struct vring_used *used_ring(void)
{
    return (struct vring_used *)(ram + USED_GPA);
}

static void set_desc(uint16_t i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next)
{
    desc_table()[i] = (struct vring_desc){ .addr = addr, .len = len, .flags = flags, .next = next };
}

/* 发布链头 head 并 kick。 */
static void kick_head(struct virtio_blk *blk, uint16_t head)
{
    struct vring_avail *a = avail_ring();
    a->ring[a->idx % QSZ] = head;
    a->idx++;
    wr(blk, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

struct result {
    int completed;  /* used.idx 前进了 1 */
    uint32_t id;
    uint32_t len;
    uint8_t status; /* 状态字节 */
};

/*
 * 用描述符 0..n-1 组一条链：hdr、数据段、status。
 * data_flags 为数据段的 WRITE 标志；返回 used ring 的结果。
 */
static struct result submit(struct virtio_blk *blk, uint32_t type, uint64_t sector,
                            const uint64_t *data_gpa, const uint32_t *data_len, int nseg,
                            uint16_t data_flags)
{
    struct virtio_blk_req_header hdr = { .type = type, .sector = sector };
    memcpy(ram + HDR_GPA, &hdr, sizeof(hdr));
    ram[STATUS_GPA] = 0xee;
    uint16_t d = 0;
    set_desc(d, HDR_GPA, sizeof(hdr), VRING_DESC_F_NEXT, 1);
    d++;
    for (int i = 0; i < nseg; i++, d++)
        set_desc(d, data_gpa[i], data_len[i], VRING_DESC_F_NEXT | data_flags, (uint16_t)(d + 1));
    set_desc(d, STATUS_GPA, 1, VRING_DESC_F_WRITE, 0);

    uint16_t before = used_ring()->idx;
    kick_head(blk, 0);
    struct result r = { 0 };
    r.completed = (uint16_t)(used_ring()->idx - before) == 1;
    struct vring_used_elem e = used_ring()->ring[before % QSZ];
    r.id = e.id;
    r.len = e.len;
    r.status = ram[STATUS_GPA];
    return r;
}

static struct result submit1(struct virtio_blk *blk, uint32_t type, uint64_t sector,
                             uint64_t gpa, uint32_t len, uint16_t flags)
{
    return submit(blk, type, sector, &gpa, &len, 1, flags);
}

static void ack_irq(struct virtio_blk *blk)
{
    wr(blk, VIRTIO_MMIO_INTERRUPT_ACK, rd(blk, VIRTIO_MMIO_INTERRUPT_STATUS, 4));
}

/* 设备在出错之后还能正常读一个扇区。 */
static int still_works(struct virtio_blk *blk)
{
    memset(ram + DATA_GPA, 0, VIRTIO_BLK_SECTOR_SIZE);
    struct result r = submit1(blk, VIRTIO_BLK_T_IN, 3, DATA_GPA, VIRTIO_BLK_SECTOR_SIZE,
                              VRING_DESC_F_WRITE);
    return r.completed && r.status == VIRTIO_BLK_S_OK &&
           memcmp(ram + DATA_GPA, image_orig + 3 * VIRTIO_BLK_SECTOR_SIZE,
                  VIRTIO_BLK_SECTOR_SIZE) == 0;
}

/* 1 */
static void test_identity_and_config(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    CHECK(rd(&blk, VIRTIO_MMIO_MAGIC_VALUE, 4) == VIRTIO_MMIO_MAGIC);
    CHECK(rd(&blk, VIRTIO_MMIO_VERSION, 4) == 2);
    CHECK(rd(&blk, VIRTIO_MMIO_DEVICE_ID, 4) == VIRTIO_ID_BLOCK_DEVICE);
    CHECK(rd(&blk, VIRTIO_MMIO_QUEUE_NUM_MAX, 4) == VIRTIO_BLK_QUEUE_SIZE);

    uint64_t f = device_features(&blk);
    CHECK(f & (1ull << VIRTIO_F_VERSION_1));
    CHECK(f & (1ull << VIRTIO_BLK_F_FLUSH));

    uint64_t cap = rd(&blk, VIRTIO_MMIO_CONFIG, 4) |
                   (uint64_t)rd(&blk, VIRTIO_MMIO_CONFIG + 4, 4) << 32;
    CHECK(cap == DISK_SECTORS);
    uint64_t cap_bytewise = 0;
    for (int i = 0; i < 8; i++)
        cap_bytewise |= (uint64_t)rd(&blk, VIRTIO_MMIO_CONFIG + i, 1) << (8 * i);
    CHECK(cap_bytewise == DISK_SECTORS);
    CHECK(rd(&blk, VIRTIO_MMIO_CONFIG + 0x14, 4) == VIRTIO_BLK_SECTOR_SIZE);
    /* 配置空间之外读 0。 */
    CHECK(rd(&blk, VIRTIO_MMIO_CONFIG + 0x200, 4) == 0);
    virtio_blk_close(&blk);
}

/* 2 */
static void test_feature_negotiation(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    CHECK(!negotiate(&blk, 1ull << VIRTIO_BLK_F_FLUSH));                 /* 缺 VERSION_1 */
    CHECK(!negotiate(&blk, (1ull << VIRTIO_F_VERSION_1) | (1ull << 29))); /* 未提供的位 */
    CHECK(!negotiate(&blk, (1ull << VIRTIO_F_VERSION_1) | (1ull << 40)));
    CHECK(negotiate(&blk, 1ull << VIRTIO_F_VERSION_1));

    /* FEATURES_OK 之后改特性无效：仍然只有 VERSION_1。 */
    write_driver_features(&blk, ~0ull);
    CHECK(blk.mmio.driver_features == 1ull << VIRTIO_F_VERSION_1);
    virtio_blk_close(&blk);
}

/* 3 */
static void test_queue_setup_validation(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    const uint32_t bad_num[] = { 0, 1, 3, 100, VIRTIO_BLK_QUEUE_SIZE * 2 };
    for (size_t i = 0; i < sizeof(bad_num) / sizeof(bad_num[0]); i++) {
        CHECK(negotiate(&blk, 1ull << VIRTIO_F_VERSION_1));
        set_queue(&blk, bad_num[i], DESC_GPA, AVAIL_GPA, USED_GPA);
        CHECK(!queue_ready(&blk));
    }
    struct {
        uint64_t desc, avail, used;
    } bad_addr[] = {
        { DESC_GPA + 8, AVAIL_GPA, USED_GPA },          /* desc 未 16 字节对齐 */
        { DESC_GPA, AVAIL_GPA + 1, USED_GPA },          /* avail 未 2 字节对齐 */
        { DESC_GPA, AVAIL_GPA, USED_GPA + 2 },          /* used 未 4 字节对齐 */
        { RAM_SIZE - 16, AVAIL_GPA, USED_GPA },         /* desc 跨出 RAM */
        { DESC_GPA, RAM_SIZE, USED_GPA },               /* avail 在 RAM 之外 */
        { DESC_GPA, AVAIL_GPA, VIRTIO_MMIO_BASE },      /* used 指向 MMIO 区 */
        { 0xfffffffffffffff0ull, AVAIL_GPA, USED_GPA }, /* 回绕 */
    };
    for (size_t i = 0; i < sizeof(bad_addr) / sizeof(bad_addr[0]); i++) {
        CHECK(negotiate(&blk, 1ull << VIRTIO_F_VERSION_1));
        set_queue(&blk, QSZ, bad_addr[i].desc, bad_addr[i].avail, bad_addr[i].used);
        CHECK(!queue_ready(&blk));
    }

    /* 队列就绪但没 DRIVER_OK：kick 不处理。 */
    CHECK(negotiate(&blk, 1ull << VIRTIO_F_VERSION_1));
    set_queue(&blk, QSZ, DESC_GPA, AVAIL_GPA, USED_GPA);
    CHECK(queue_ready(&blk));
    struct result r = submit1(&blk, VIRTIO_BLK_T_IN, 0, DATA_GPA, 512, VRING_DESC_F_WRITE);
    CHECK(!r.completed);

    /* 只有队列 0。 */
    wr(&blk, VIRTIO_MMIO_QUEUE_SEL, 1);
    CHECK(rd(&blk, VIRTIO_MMIO_QUEUE_NUM_MAX, 4) == 0);
    virtio_blk_close(&blk);
}

/* 4 */
static void test_read_write_flush_get_id(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    bring_up(&blk);

    /* 两段读：扇区 5..7 分到 512 + 1024 字节两个 buffer。 */
    uint64_t gpa[2] = { DATA_GPA, DATA_GPA + 0x4000 };
    uint32_t len[2] = { 512, 1024 };
    struct result r = submit(&blk, VIRTIO_BLK_T_IN, 5, gpa, len, 2, VRING_DESC_F_WRITE);
    CHECK(r.completed && r.id == 0 && r.status == VIRTIO_BLK_S_OK);
    CHECK(r.len == 512 + 1024 + 1);
    CHECK(memcmp(ram + DATA_GPA, image_orig + 5 * 512, 512) == 0);
    CHECK(memcmp(ram + DATA_GPA + 0x4000, image_orig + 6 * 512, 1024) == 0);

    /* 最后一个扇区。 */
    r = submit1(&blk, VIRTIO_BLK_T_IN, DISK_SECTORS - 1, DATA_GPA, 512, VRING_DESC_F_WRITE);
    CHECK(r.completed && r.status == VIRTIO_BLK_S_OK);
    CHECK(memcmp(ram + DATA_GPA, image_orig + DISK_BYTES - 512, 512) == 0);

    /* 写扇区 10，再从文件里读回来。 */
    memset(ram + DATA_GPA, 0x5a, 512);
    r = submit1(&blk, VIRTIO_BLK_T_OUT, 10, DATA_GPA, 512, 0);
    CHECK(r.completed && r.status == VIRTIO_BLK_S_OK && r.len == 1);
    uint8_t buf[512];
    int fd = open(image_path, O_RDONLY);
    CHECK(pread(fd, buf, 512, 10 * 512) == 512);
    close(fd);
    uint8_t expect[512];
    memset(expect, 0x5a, sizeof(expect));
    CHECK(memcmp(buf, expect, 512) == 0);

    r = submit(&blk, VIRTIO_BLK_T_FLUSH, 0, NULL, NULL, 0, 0);
    CHECK(r.completed && r.status == VIRTIO_BLK_S_OK && r.len == 1);

    memset(ram + DATA_GPA, 0, 64);
    r = submit1(&blk, VIRTIO_BLK_T_GET_ID, 0, DATA_GPA, VIRTIO_BLK_ID_BYTES, VRING_DESC_F_WRITE);
    CHECK(r.completed && r.status == VIRTIO_BLK_S_OK);
    CHECK(r.len == VIRTIO_BLK_ID_BYTES + 1);
    CHECK(ram[DATA_GPA] != 0 && ram[DATA_GPA + VIRTIO_BLK_ID_BYTES] == 0);

    CHECK(blk.stats.reads == 2 && blk.stats.writes == 1 && blk.stats.flushes == 1);
    CHECK(blk.stats.errors == 0);
    virtio_blk_close(&blk);
}

/* 5、6、7、8、12：全部 IOERR，镜像不变，设备继续可用。 */
static void test_bad_requests_do_no_io(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    bring_up(&blk);

    struct {
        const char *what;
        uint32_t type;
        uint64_t sector;
        uint64_t gpa[2];
        uint32_t len[2];
        int nseg;
        uint16_t flags;
    } cases[] = {
        { "write from gpa outside ram", VIRTIO_BLK_T_OUT, 0, { RAM_SIZE }, { 512 }, 1, 0 },
        { "write straddling ram end", VIRTIO_BLK_T_OUT, 0, { RAM_SIZE - 256 }, { 512 }, 1, 0 },
        { "write gpa wraps", VIRTIO_BLK_T_OUT, 0, { 0xffffffffffffff00ull }, { 512 }, 1, 0 },
        { "read into gpa outside ram", VIRTIO_BLK_T_IN, 0, { RAM_SIZE + 4096 }, { 512 }, 1,
          VRING_DESC_F_WRITE },
        { "write past end", VIRTIO_BLK_T_OUT, DISK_SECTORS, { DATA_GPA }, { 512 }, 1, 0 },
        { "write partly past end", VIRTIO_BLK_T_OUT, DISK_SECTORS - 1, { DATA_GPA }, { 1024 }, 1,
          0 },
        { "sector*512 overflows", VIRTIO_BLK_T_OUT, 0x0080000000000000ull, { DATA_GPA }, { 512 },
          1, 0 },
        { "sector max", VIRTIO_BLK_T_IN, UINT64_MAX, { DATA_GPA }, { 512 }, 1,
          VRING_DESC_F_WRITE },
        { "read past end", VIRTIO_BLK_T_IN, DISK_SECTORS, { DATA_GPA }, { 512 }, 1,
          VRING_DESC_F_WRITE },
        { "read into read-only buffer", VIRTIO_BLK_T_IN, 0, { DATA_GPA }, { 512 }, 1, 0 },
        { "write from writable buffer", VIRTIO_BLK_T_OUT, 0, { DATA_GPA }, { 512 }, 1,
          VRING_DESC_F_WRITE },
        { "length not sector multiple", VIRTIO_BLK_T_OUT, 0, { DATA_GPA }, { 100 }, 1, 0 },
        { "second segment outside ram", VIRTIO_BLK_T_OUT, 0, { DATA_GPA, RAM_SIZE }, { 512, 512 },
          2, 0 },
        { "read with no data segment", VIRTIO_BLK_T_IN, 0, { 0 }, { 0 }, 0, 0 },
    };
    memset(ram + DATA_GPA, 0xa5, 0x1000);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct result r = submit(&blk, cases[i].type, cases[i].sector, cases[i].gpa, cases[i].len,
                                 cases[i].nseg, cases[i].flags);
        if (!r.completed || r.status != VIRTIO_BLK_S_IOERR || !image_equals_orig())
            fprintf(stderr, "  case: %s\n", cases[i].what);
        CHECK(r.completed && r.status == VIRTIO_BLK_S_IOERR);
        CHECK(r.len == 1);
        CHECK(image_equals_orig());
    }
    /* 读越界时也不能写客户机 buffer。 */
    for (int i = 0; i < 0x1000; i++)
        if (ram[DATA_GPA + i] != 0xa5) {
            CHECK(!"guest buffer modified by failed request");
            break;
        }
    CHECK(still_works(&blk));
    CHECK(blk.stats.errors == sizeof(cases) / sizeof(cases[0]));
    virtio_blk_close(&blk);
}

/* 9、10：链结构非法。都要完成（driver 不能挂住），设备之后仍可用。 */
static void test_malformed_chains_complete(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    bring_up(&blk);
    struct virtio_blk_req_header hdr = { .type = VIRTIO_BLK_T_IN, .sector = 0 };
    memcpy(ram + HDR_GPA, &hdr, sizeof(hdr));

    /* 请求头只有 8 字节：状态位置合法，应写 IOERR。 */
    ram[STATUS_GPA] = 0xee;
    set_desc(0, HDR_GPA, 8, VRING_DESC_F_NEXT, 1);
    set_desc(1, DATA_GPA, 512, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 2);
    set_desc(2, STATUS_GPA, 1, VRING_DESC_F_WRITE, 0);
    uint16_t before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);
    CHECK(ram[STATUS_GPA] == VIRTIO_BLK_S_IOERR);

    /* 请求头可写（driver 让 device 写请求头？）：IOERR。 */
    ram[STATUS_GPA] = 0xee;
    set_desc(0, HDR_GPA, 16, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 1);
    before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);
    CHECK(ram[STATUS_GPA] == VIRTIO_BLK_S_IOERR);

    /* 状态描述符只读：不能写状态，完成项 len = 0。 */
    set_desc(0, HDR_GPA, 16, VRING_DESC_F_NEXT, 1);
    set_desc(2, STATUS_GPA, 1, 0, 0);
    ram[STATUS_GPA] = 0xee;
    before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);
    CHECK(used_ring()->ring[before % QSZ].len == 0);
    CHECK(ram[STATUS_GPA] == 0xee);

    /* 状态描述符长度 0。 */
    set_desc(2, STATUS_GPA, 0, VRING_DESC_F_WRITE, 0);
    before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);
    CHECK(used_ring()->ring[before % QSZ].len == 0);

    /* 状态描述符在 RAM 之外。 */
    set_desc(2, RAM_SIZE + 10, 1, VRING_DESC_F_WRITE, 0);
    before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);
    CHECK(used_ring()->ring[before % QSZ].len == 0);

    /* 只有一个描述符（没有状态）。 */
    set_desc(0, HDR_GPA, 16, 0, 0);
    before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);
    CHECK(used_ring()->ring[before % QSZ].len == 0);

    /* 环：0 → 1 → 0。 */
    set_desc(0, HDR_GPA, 16, VRING_DESC_F_NEXT, 1);
    set_desc(1, STATUS_GPA, 1, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 0);
    before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);
    CHECK(used_ring()->ring[before % QSZ].id == 0 && used_ring()->ring[before % QSZ].len == 0);

    /* next 越界。 */
    set_desc(1, STATUS_GPA, 1, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 200);
    before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);

    /* INDIRECT。 */
    set_desc(0, HDR_GPA, 16, VRING_DESC_F_INDIRECT, 0);
    before = used_ring()->idx;
    kick_head(&blk, 0);
    CHECK((uint16_t)(used_ring()->idx - before) == 1);

    /* 16：未知类型 → UNSUPP。 */
    struct result r = submit(&blk, 0x1234, 0, NULL, NULL, 0, 0);
    CHECK(r.completed && r.status == VIRTIO_BLK_S_UNSUPP && r.len == 1);

    CHECK(!(status_reg(&blk) & VIRTIO_STATUS_DEVICE_NEEDS_RESET));
    CHECK(still_works(&blk));
    CHECK(image_equals_orig());
    virtio_blk_close(&blk);
}

/* 11 */
static void test_broken_avail_ring_needs_reset(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    bring_up(&blk);

    /* avail.idx 一次跳过 qsz + 1。 */
    avail_ring()->idx = QSZ + 1;
    uint16_t before = used_ring()->idx;
    wr(&blk, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    CHECK(used_ring()->idx == before);
    CHECK(status_reg(&blk) & VIRTIO_STATUS_DEVICE_NEEDS_RESET);
    CHECK(rd(&blk, VIRTIO_MMIO_INTERRUPT_STATUS, 4) & VIRTIO_MMIO_INT_CONFIG);
    CHECK(irq_level == 1);

    /* NEEDS_RESET 期间 kick 无效。 */
    wr(&blk, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    CHECK(used_ring()->idx == before);

    /* 复位后中断线拉低，重新初始化即可工作。 */
    wr(&blk, VIRTIO_MMIO_STATUS, 0);
    CHECK(irq_level == 0);
    CHECK(status_reg(&blk) == 0);
    CHECK(rd(&blk, VIRTIO_MMIO_INTERRUPT_STATUS, 4) == 0);
    memset(ram + AVAIL_GPA, 0, 0x1000);
    memset(ram + USED_GPA, 0, 0x1000);
    bring_up(&blk);
    CHECK(still_works(&blk));

    /* 链头越界：无法放进 used ring，也只能 NEEDS_RESET。 */
    struct vring_avail *a = avail_ring();
    a->ring[a->idx % QSZ] = QSZ + 3;
    a->idx++;
    before = used_ring()->idx;
    wr(&blk, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    CHECK(used_ring()->idx == before);
    CHECK(status_reg(&blk) & VIRTIO_STATUS_DEVICE_NEEDS_RESET);
    virtio_blk_close(&blk);
}

/* 13 */
static void test_interrupts(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    bring_up(&blk);
    CHECK(irq_level == 0);

    CHECK(still_works(&blk));
    CHECK(irq_level == 1 && irq_raises == 1);
    CHECK(rd(&blk, VIRTIO_MMIO_INTERRUPT_STATUS, 4) == VIRTIO_MMIO_INT_VRING);
    /* ACK 一个没置位的位不影响。 */
    wr(&blk, VIRTIO_MMIO_INTERRUPT_ACK, VIRTIO_MMIO_INT_CONFIG);
    CHECK(irq_level == 1);
    ack_irq(&blk);
    CHECK(irq_level == 0);
    CHECK(rd(&blk, VIRTIO_MMIO_INTERRUPT_STATUS, 4) == 0);

    /* 未 ACK 时第二次完成：线保持高，不重复计数。 */
    CHECK(still_works(&blk));
    CHECK(still_works(&blk));
    CHECK(irq_level == 1 && irq_raises == 2);
    ack_irq(&blk);

    /* driver 设置 NO_INTERRUPT：完成但不发中断。 */
    avail_ring()->flags = VRING_AVAIL_F_NO_INTERRUPT;
    CHECK(still_works(&blk));
    CHECK(irq_level == 0 && irq_raises == 2);
    CHECK(rd(&blk, VIRTIO_MMIO_INTERRUPT_STATUS, 4) == 0);
    avail_ring()->flags = 0;

    /* 没有新请求的 kick 不发中断。 */
    wr(&blk, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    CHECK(irq_level == 0 && irq_raises == 2);

    /* 复位拉低中断线。 */
    CHECK(still_works(&blk));
    CHECK(irq_level == 1);
    wr(&blk, VIRTIO_MMIO_STATUS, 0);
    CHECK(irq_level == 0);
    virtio_blk_close(&blk);
}

/* 14 */
static void test_bad_mmio_accesses(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    bring_up(&blk);
    uint32_t status = status_reg(&blk);

    uint8_t buf[8];
    memset(buf, 0xcc, sizeof(buf));
    virtio_mmio_access(&blk.mmio, VIRTIO_MMIO_MAGIC_VALUE, buf, 2, 0); /* 宽度不对 */
    CHECK(buf[0] == 0 && buf[1] == 0);
    memset(buf, 0xcc, sizeof(buf));
    virtio_mmio_access(&blk.mmio, VIRTIO_MMIO_MAGIC_VALUE + 1, buf, 4, 0); /* 未对齐 */
    CHECK(buf[0] == 0 && buf[3] == 0);
    virtio_mmio_access(&blk.mmio, VIRTIO_MMIO_SIZE, buf, 4, 0); /* 页外 */
    virtio_mmio_access(&blk.mmio, UINT64_MAX - 1, buf, 4, 0);
    virtio_mmio_access(&blk.mmio, VIRTIO_MMIO_CONFIG + 0x1000, buf, 8, 0);
    virtio_mmio_access(&blk.mmio, 0, buf, 0, 0);
    virtio_mmio_access(&blk.mmio, 0, buf, 3, 0);

    /* 写只读寄存器、写配置空间、错宽度写 Status：状态不变。 */
    wr(&blk, VIRTIO_MMIO_MAGIC_VALUE, 0);
    wr(&blk, VIRTIO_MMIO_DEVICE_ID, 7);
    wr(&blk, VIRTIO_MMIO_CONFIG, 0);
    uint16_t zero16 = 0;
    virtio_mmio_access(&blk.mmio, VIRTIO_MMIO_STATUS, (uint8_t *)&zero16, 2, 1);
    virtio_mmio_access(&blk.mmio, VIRTIO_MMIO_QUEUE_NUM, (uint8_t *)&zero16, 2, 1);
    CHECK(rd(&blk, VIRTIO_MMIO_MAGIC_VALUE, 4) == VIRTIO_MMIO_MAGIC);
    CHECK(rd(&blk, VIRTIO_MMIO_DEVICE_ID, 4) == VIRTIO_ID_BLOCK_DEVICE);
    CHECK(rd(&blk, VIRTIO_MMIO_CONFIG, 4) == DISK_SECTORS);
    CHECK(status_reg(&blk) == status);
    CHECK(blk.mmio.stats.bad_accesses >= 8);

    /* 就绪之后再改 QueueNum/地址无效。 */
    set_queue(&blk, 4, DESC_GPA + 0x100, AVAIL_GPA, USED_GPA);
    CHECK(blk.mmio.vq.qsz == QSZ && blk.mmio.vq.desc == desc_table());
    CHECK(still_works(&blk));
    virtio_blk_close(&blk);
}

/* 15 */
static void test_truncated_image(void)
{
    struct virtio_blk blk;
    open_device(&blk);
    bring_up(&blk);
    CHECK(truncate(image_path, 8 * VIRTIO_BLK_SECTOR_SIZE) == 0);
    memset(ram + DATA_GPA, 0xa5, 1024);
    struct result r = submit1(&blk, VIRTIO_BLK_T_IN, 20, DATA_GPA, 512, VRING_DESC_F_WRITE);
    CHECK(r.completed && r.status == VIRTIO_BLK_S_IOERR);
    /* 跨 EOF 的读也是错误。 */
    r = submit1(&blk, VIRTIO_BLK_T_IN, 7, DATA_GPA, 1024, VRING_DESC_F_WRITE);
    CHECK(r.completed && r.status == VIRTIO_BLK_S_IOERR);
    virtio_blk_close(&blk);
}

/* 17 */
static void test_open_rejects_bad_images(void)
{
    struct virtio_blk a, b;
    CHECK(virtio_blk_open(&a, "/nonexistent/svmm-disk.img") < 0);
    virtio_blk_close(&a);
    CHECK(virtio_blk_open(&a, "/tmp") < 0);
    virtio_blk_close(&a);

    CHECK(truncate(image_path, 100) == 0);
    CHECK(virtio_blk_open(&a, image_path) < 0);
    virtio_blk_close(&a);

    make_image();
    CHECK(virtio_blk_open(&a, image_path) == 0);
    CHECK(virtio_blk_open(&b, image_path) < 0); /* 已被锁住 */
    virtio_blk_close(&b);
    virtio_blk_close(&a);
    CHECK(virtio_blk_open(&b, image_path) == 0); /* 关闭后可以再打开 */
    CHECK(b.config.capacity == DISK_SECTORS);
    virtio_blk_close(&b);

    /* 末尾不足一个扇区的部分不可见。 */
    CHECK(truncate(image_path, DISK_BYTES + 100) == 0);
    CHECK(virtio_blk_open(&a, image_path) == 0);
    CHECK(a.config.capacity == DISK_SECTORS);
    virtio_blk_close(&a);
}

int main(void)
{
    int fd = mkstemp(image_path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);
    ram = malloc(RAM_SIZE);
    if (!ram)
        return 1;

    test_identity_and_config();
    test_feature_negotiation();
    test_queue_setup_validation();
    test_read_write_flush_get_id();
    test_bad_requests_do_no_io();
    test_malformed_chains_complete();
    test_broken_avail_ring_needs_reset();
    test_interrupts();
    test_bad_mmio_accesses();
    test_truncated_image();
    test_open_rejects_bad_images();

    unlink(image_path);
    free(ram);
    if (failures) {
        fprintf(stderr, "test_virtio_blk: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_virtio_blk: all tests passed\n");
    return 0;
}

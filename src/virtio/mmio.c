#include "virtio/mmio.h"

#include <string.h>

/* driver 能写的 Status 位；DEVICE_NEEDS_RESET 只由 device 设置。 */
#define STATUS_DRIVER_BITS                                                         \
    (VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | \
     VIRTIO_STATUS_DRIVER_OK | VIRTIO_STATUS_FAILED)

void *virtio_mmio_gpa(const struct virtio_mmio *dev, uint64_t gpa, uint64_t len)
{
    /* 写成减法，gpa + len 溢出回绕时也不会误判为在范围内。 */
    if (gpa > dev->mem_size || len > dev->mem_size - gpa)
        return NULL;
    return dev->mem + gpa;
}

static void update_irq(struct virtio_mmio *dev)
{
    int level = dev->interrupt_status != 0;
    if (level == dev->irq_level)
        return;
    dev->irq_level = level;
    if (level)
        dev->stats.interrupts++;
    if (dev->set_irq)
        dev->set_irq(dev->irq_opaque, level);
}

/* Status 写 0：回到刚上电的状态，包括拉低中断线。 */
static void reset(struct virtio_mmio *dev)
{
    dev->status = 0;
    dev->device_features_sel = 0;
    dev->driver_features_sel = 0;
    dev->driver_features = 0;
    dev->queue_sel = 0;
    dev->queue_num = 0;
    dev->queue_desc = 0;
    dev->queue_avail = 0;
    dev->queue_used = 0;
    dev->queue_ready = 0;
    dev->vq = (struct virtq){ 0 };
    dev->interrupt_status = 0;
    update_irq(dev);
}

void virtio_mmio_init(struct virtio_mmio *dev, const struct virtio_mmio_backend *backend,
                      void *opaque, uint8_t *mem, uint64_t mem_size,
                      void (*set_irq)(void *opaque, int level), void *irq_opaque)
{
    *dev = (struct virtio_mmio){
        .backend = backend,
        .opaque = opaque,
        .mem = mem,
        .mem_size = mem_size,
        .set_irq = set_irq,
        .irq_opaque = irq_opaque,
    };
    reset(dev);
}

/*
 * device 发现自己无法继续（例如 driver 把 avail ring 写坏了）：置 NEEDS_RESET，
 * 并发一个配置变更中断让 driver 注意到（§2.1.2）。之后 kick 都被忽略，直到复位。
 */
static void needs_reset(struct virtio_mmio *dev)
{
    dev->status |= VIRTIO_STATUS_DEVICE_NEEDS_RESET;
    dev->interrupt_status |= VIRTIO_MMIO_INT_CONFIG;
    update_irq(dev);
}

/*
 * QueueReady=1：driver 声明队列配置完毕。在这里一次性检查 driver 写的所有值，
 * 通过之后才让 vq 指向客户机内存；任何一项不对都保持 ready=0，
 * Linux 读回 0 会放弃这个设备。
 */
static int queue_setup_valid(const struct virtio_mmio *dev)
{
    uint32_t num = dev->queue_num;
    /*
     * QueueNum 决定三块 ring 的布局，driver 和 device 必须一致。不合法时
     * 拒绝而不是截断：截断后 device 按另一个大小解析 ring，下标全部错位。
     */
    if (num < 2 || num > dev->backend->queue_num_max || (num & (num - 1)))
        return 0;
    /* §2.6 的对齐要求。 */
    if ((dev->queue_desc & 15) || (dev->queue_avail & 1) || (dev->queue_used & 3))
        return 0;
    return virtio_mmio_gpa(dev, dev->queue_desc, virtq_desc_bytes(num)) &&
           virtio_mmio_gpa(dev, dev->queue_avail, virtq_avail_bytes(num)) &&
           virtio_mmio_gpa(dev, dev->queue_used, virtq_used_bytes(num));
}

static void queue_set_ready(struct virtio_mmio *dev, uint32_t value)
{
    if (dev->queue_sel != 0)
        return;
    if (value == 0) {
        dev->queue_ready = 0;
        return;
    }
    if (value != 1 || dev->queue_ready || !(dev->status & VIRTIO_STATUS_FEATURES_OK) ||
        !queue_setup_valid(dev))
        return;
    if (virtq_init_shared(&dev->vq, dev->queue_num,
                          virtio_mmio_gpa(dev, dev->queue_desc, 0),
                          virtio_mmio_gpa(dev, dev->queue_avail, 0),
                          virtio_mmio_gpa(dev, dev->queue_used, 0)) < 0)
        return;
    dev->queue_ready = 1;
}

/*
 * driver 写 QueueNotify（doorbell）。这次 VM exit 发生在 vCPU 线程上，
 * 设备在这里同步处理完所有请求再返回客户机，所以 kick 返回时 used ring
 * 已经更新好了。
 */
static void queue_notify(struct virtio_mmio *dev, uint32_t queue)
{
    const uint32_t ready = VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
    if (queue != 0 || !dev->queue_ready || (dev->status & ready) != ready ||
        (dev->status & (VIRTIO_STATUS_DEVICE_NEEDS_RESET | VIRTIO_STATUS_FAILED)))
        return;
    dev->stats.notifies++;
    uint16_t before = dev->vq.next_used_idx;
    int rc = dev->backend->process_queue(dev, &dev->vq);
    if (dev->vq.next_used_idx != before) {
        /*
         * used.idx 的发布（store）和读 avail.flags（load）之间需要全屏障：
         * 否则 load 可能提前到 store 之前，读到 driver 在看到新 used.idx
         * 之前设置的 NO_INTERRUPT，漏发中断。
         */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (virtq_needs_interrupt(&dev->vq)) {
            dev->interrupt_status |= VIRTIO_MMIO_INT_VRING;
            update_irq(dev);
        }
    }
    if (rc < 0)
        needs_reset(dev);
}

static void write_status(struct virtio_mmio *dev, uint32_t value)
{
    if (value == 0) {
        reset(dev);
        return;
    }
    uint32_t old = dev->status;
    uint32_t next = (value & STATUS_DRIVER_BITS) | (old & VIRTIO_STATUS_DEVICE_NEEDS_RESET);
    /*
     * FEATURES_OK 是 device 对 driver 所选特性的确认：只接受 device 提供过的位，
     * 并且必须包含 VERSION_1（本设备只实现 modern 接口）。不接受时不保留这一位，
     * driver 读回发现后会放弃（§3.1.1 第 6 步）。一旦接受就不能再撤销。
     */
    if ((next & VIRTIO_STATUS_FEATURES_OK) && !(old & VIRTIO_STATUS_FEATURES_OK)) {
        uint64_t offered = dev->backend->features;
        if ((dev->driver_features & ~offered) ||
            !(dev->driver_features & (1ull << VIRTIO_F_VERSION_1)))
            next &= ~VIRTIO_STATUS_FEATURES_OK;
    }
    next |= old & VIRTIO_STATUS_FEATURES_OK;
    dev->status = next;
}

static void write_driver_features(struct virtio_mmio *dev, uint32_t value)
{
    /* FEATURES_OK 之后特性已经冻结。 */
    if (dev->status & VIRTIO_STATUS_FEATURES_OK)
        return;
    if (dev->driver_features_sel == 0)
        dev->driver_features = (dev->driver_features & ~0xffffffffull) | value;
    else if (dev->driver_features_sel == 1)
        dev->driver_features = (dev->driver_features & 0xffffffffull) | (uint64_t)value << 32;
}

/* 地址寄存器是 64 位值的低/高 32 位；只能在队列就绪之前写。 */
static void write_queue_addr(struct virtio_mmio *dev, uint64_t *addr, int high, uint32_t value)
{
    if (dev->queue_sel != 0 || dev->queue_ready)
        return;
    if (high)
        *addr = (*addr & 0xffffffffull) | (uint64_t)value << 32;
    else
        *addr = (*addr & ~0xffffffffull) | value;
}

static uint32_t read_reg(struct virtio_mmio *dev, uint64_t offset)
{
    const struct virtio_mmio_backend *be = dev->backend;
    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        return VIRTIO_MMIO_MAGIC;
    case VIRTIO_MMIO_VERSION:
        return 2;
    case VIRTIO_MMIO_DEVICE_ID:
        return be->device_id;
    case VIRTIO_MMIO_VENDOR_ID:
        return VIRTIO_MMIO_VENDOR;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        if (dev->device_features_sel > 1)
            return 0;
        return (uint32_t)(be->features >> (32 * dev->device_features_sel));
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        return dev->queue_sel == 0 ? be->queue_num_max : 0;
    case VIRTIO_MMIO_QUEUE_READY:
        return dev->queue_sel == 0 ? (uint32_t)dev->queue_ready : 0;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        return dev->interrupt_status;
    case VIRTIO_MMIO_STATUS:
        return dev->status;
    case VIRTIO_MMIO_CONFIG_GENERATION:
        return 0; /* 配置空间从不改变 */
    /* 共享内存区长度读出全 1 表示“没有这个区”。 */
    case VIRTIO_MMIO_SHM_LEN_LOW:
    case VIRTIO_MMIO_SHM_LEN_HIGH:
    case VIRTIO_MMIO_SHM_BASE_LOW:
    case VIRTIO_MMIO_SHM_BASE_HIGH:
        return 0xffffffffu;
    default:
        /* 只写寄存器和保留偏移读 0。 */
        return 0;
    }
}

/* 返回 0 表示写的是可写寄存器。 */
static int write_reg(struct virtio_mmio *dev, uint64_t offset, uint32_t value)
{
    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        dev->device_features_sel = value;
        return 0;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        write_driver_features(dev, value);
        return 0;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        dev->driver_features_sel = value;
        return 0;
    case VIRTIO_MMIO_QUEUE_SEL:
        dev->queue_sel = value;
        return 0;
    case VIRTIO_MMIO_QUEUE_NUM:
        if (dev->queue_sel == 0 && !dev->queue_ready)
            dev->queue_num = value;
        return 0;
    case VIRTIO_MMIO_QUEUE_READY:
        queue_set_ready(dev, value);
        return 0;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        queue_notify(dev, value);
        return 0;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        dev->interrupt_status &= ~value;
        update_irq(dev);
        return 0;
    case VIRTIO_MMIO_STATUS:
        write_status(dev, value);
        return 0;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        write_queue_addr(dev, &dev->queue_desc, offset == VIRTIO_MMIO_QUEUE_DESC_HIGH, value);
        return 0;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        write_queue_addr(dev, &dev->queue_avail, offset == VIRTIO_MMIO_QUEUE_AVAIL_HIGH, value);
        return 0;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        write_queue_addr(dev, &dev->queue_used, offset == VIRTIO_MMIO_QUEUE_USED_HIGH, value);
        return 0;
    case VIRTIO_MMIO_SHM_SEL:
        return 0;
    default:
        return -1;
    }
}

void virtio_mmio_access(struct virtio_mmio *dev, uint64_t offset, uint8_t *data,
                        uint32_t len, int is_write)
{
    if (is_write)
        dev->stats.writes++;
    else
        dev->stats.reads++;

    int valid_len = len == 1 || len == 2 || len == 4 || len == 8;
    if (!valid_len || offset >= VIRTIO_MMIO_SIZE || len > VIRTIO_MMIO_SIZE - offset)
        goto bad;

    if (offset >= VIRTIO_MMIO_CONFIG) {
        /*
         * 设备配置空间：driver 按字段宽度读（Linux 用 readb/readw/readl，64 位
         * 字段拆成两次 readl），所以允许任意宽度。超出配置结构的部分读 0。
         * 本设备的配置对 driver 只读。
         */
        if (is_write)
            goto bad;
        uint64_t pos = offset - VIRTIO_MMIO_CONFIG;
        const struct virtio_mmio_backend *be = dev->backend;
        memset(data, 0, len);
        if (pos < be->config_size) {
            uint64_t n = be->config_size - pos;
            memcpy(data, (const uint8_t *)be->config + pos, n < len ? n : len);
        }
        return;
    }

    /* 其余寄存器都是 32 位，规范要求 driver 只做 4 字节对齐访问。 */
    if (len != 4 || (offset & 3))
        goto bad;
    if (is_write) {
        uint32_t value;
        memcpy(&value, data, 4);
        if (write_reg(dev, offset, value) < 0)
            dev->stats.bad_accesses++;
    } else {
        uint32_t value = read_reg(dev, offset);
        memcpy(data, &value, 4);
    }
    return;

bad:
    dev->stats.bad_accesses++;
    if (!is_write && len <= 8)
        memset(data, 0, len);
}

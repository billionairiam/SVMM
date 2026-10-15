#include "virtio/mmio.h"

#include <string.h>

/*
 * virtio-mmio 设备端的完整生命周期。左边是客户机 Linux 的
 * drivers/virtio/virtio_mmio.c（以及 virtio_ring.c、virtio_blk.c），右边是本文件：
 *
 *   客户机 driver                              VMM（本文件）
 *   ─────────────────────────────────────     ─────────────────────────────────
 *   virtio_mmio_probe()
 *     读 MagicValue/Version/DeviceID/VendorID → read_reg()：返回 "virt"、2、2(blk)…
 *   vm_reset(): 写 Status=0                  → reset()：清空所有状态、拉低中断线
 *   写 Status |= ACKNOWLEDGE | DRIVER         → write_status()
 *   vm_get_features():
 *     写 DeviceFeaturesSel=1，读 DeviceFeatures → 高 32 位（VERSION_1 在 bit 32）
 *     写 DeviceFeaturesSel=0，读 DeviceFeatures → 低 32 位
 *   vm_finalize_features():
 *     写 DriverFeaturesSel/DriverFeatures     → write_driver_features()
 *   写 Status |= FEATURES_OK，再读回           → write_status() 校验后决定保不保留
 *   读配置空间（capacity、seg_max…）           → virtio_mmio_access() 的配置空间分支
 *   vm_setup_vq()，对每个队列（blk 1 个，net 是 RX=0、TX=1）:
 *     写 QueueSel=i，读 QueueNumMax          → 128（不存在的队列读 0）
 *     写 QueueNum，写 Queue{Desc,Avail,Used}{Low,High}
 *     写 QueueReady=1                        → queue_set_ready()：校验，vq 指向客户机内存
 *   写 Status |= DRIVER_OK                   → 设备开始工作
 *
 *   ── 之后每个 I/O 请求 ──
 *   virtio_blk 填描述符、avail ring，发布 avail.idx（客户机内存，不退出）
 *   vm_notify(): 写 QueueNotify=0             → queue_notify()（KVM_EXIT_MMIO）
 *                                                 → blk.c 处理请求、写 used ring
 *                                                 → InterruptStatus |= VRING，拉高 GSI 5
 *   客户机收到中断，vm_interrupt():
 *     读 InterruptStatus                     → read_reg()
 *     写 InterruptACK                        → 清位，全清后拉低 GSI 5
 *     vring_interrupt() 回收 used ring，唤醒等待这个请求的进程
 *
 * 注意：每一次寄存器读写都是一次 VM exit（几微秒），而数据本身（ring 和 buffer）
 * 在共享内存里，不产生 exit。所以 virtio 的设计目标就是让每个请求只需要很少的
 * 寄存器访问：理想情况下一次 kick + 一次中断（带 ACK）。
 */

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

/*
 * 让中断线的电平跟随 InterruptStatus：有任何未应答的原因就保持高电平，
 * 全部被 ACK 后拉低。这就是“电平触发”（DSDT 里声明为 Level, ActiveHigh）。
 *
 * 为什么用电平触发？“线的状态 = 是否还有未应答的原因”，不依赖客户机恰好捕获
 * 某个边沿：driver 处理中断期间又有新请求完成，InterruptStatus 仍不为 0，线就
 * 一直是高，I/O APIC 在客户机 EOI 后会再投递一次，不会丢。电平触发的线也可以
 * 被多个设备共享（Linux 用 IRQF_SHARED 申请 virtio-mmio 的中断）。
 *
 * 只在电平变化时调用 set_irq，避免每次寄存器访问都多一次 KVM_IRQ_LINE。
 */
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

/*
 * Status 写 0：回到刚上电的状态，包括拉低中断线。
 * 只清 VMM 侧的状态，不碰客户机内存里的 ring：那是 driver 的内存，复位后
 * driver 会重新分配并初始化。vq 清零后 device 不再持有指向它的指针。
 */
static void reset(struct virtio_mmio *dev)
{
    dev->status = 0;
    dev->device_features_sel = 0;
    dev->driver_features_sel = 0;
    dev->driver_features = 0;
    dev->queue_sel = 0;
    memset(dev->queues, 0, sizeof(dev->queues));
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
    pthread_mutex_init(&dev->lock, NULL);
    reset(dev);
}

/* QueueSel 选中的队列；选了不存在的队列时返回 NULL，相关寄存器读 0、写忽略。 */
static struct virtio_mmio_queue *selected_queue(struct virtio_mmio *dev)
{
    if (dev->queue_sel >= dev->backend->num_queues)
        return NULL;
    return &dev->queues[dev->queue_sel];
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
static int queue_setup_valid(const struct virtio_mmio *dev, const struct virtio_mmio_queue *q)
{
    uint32_t num = q->num;
    /*
     * QueueNum 决定三块 ring 的布局，driver 和 device 必须一致。不合法时
     * 拒绝而不是截断：截断后 device 按另一个大小解析 ring，下标全部错位。
     */
    if (num < 2 || num > dev->backend->queue_num_max || (num & (num - 1)))
        return 0;
    /*
     * §2.6 的对齐要求：desc 16 字节、avail 2 字节、used 4 字节。
     * 这关系到 VMM 自己的安全：未对齐的 GPA 换成宿主指针后，按 struct 访问
     * 就是未定义行为。
     */
    if ((q->desc & 15) || (q->avail & 1) || (q->used & 3))
        return 0;
    /*
     * 三块 ring 都必须整块落在客户机 RAM 里。之后 virtq.c 会用
     * “基址 + 下标”直接访问它们，只在这里做一次范围检查；下标本身由
     * slot() 的掩码限制在 [0, qsz)，所以不会越出这里检查过的范围。
     */
    return virtio_mmio_gpa(dev, q->desc, virtq_desc_bytes(num)) &&
           virtio_mmio_gpa(dev, q->avail, virtq_avail_bytes(num)) &&
           virtio_mmio_gpa(dev, q->used, virtq_used_bytes(num));
}

/*
 * 写 QueueReady。QueueSel 指向不存在的队列时写被忽略（QueueNumMax 对它们
 * 读 0，driver 就知道这些队列不存在）。写 0 只是清 ready 标志；写 1 时检查：
 * 未就绪、已经 FEATURES_OK（规范要求先协商特性再配队列）、配置合法。
 */
static void queue_set_ready(struct virtio_mmio *dev, uint32_t value)
{
    struct virtio_mmio_queue *q = selected_queue(dev);
    if (!q)
        return;
    if (value == 0) {
        q->ready = 0;
        return;
    }
    if (value != 1 || q->ready || !(dev->status & VIRTIO_STATUS_FEATURES_OK) ||
        !queue_setup_valid(dev, q))
        return;
    /*
     * GPA → 宿主指针，len 传 0 是因为范围已经在 queue_setup_valid() 里查过。
     * 从这一刻起 q->vq 的三个指针直接指向客户机内存：客户机写 ring，
     * VMM 立刻就能读到，反之亦然，这就是“共享内存”的含义。
     */
    if (virtq_init_shared(&q->vq, q->num, virtio_mmio_gpa(dev, q->desc, 0),
                          virtio_mmio_gpa(dev, q->avail, 0),
                          virtio_mmio_gpa(dev, q->used, 0)) < 0)
        return;
    q->ready = 1;
}

/*
 * 队列“活着”才允许 device 碰它的 ring：设备没准备好（DRIVER_OK 之前 driver 可能
 * 还在填 ring）、已出错、driver 自己放弃了（FAILED）、或者队列没 ready（复位后
 * 那块内存可能已经被 driver 释放）时一律不处理。
 */
static struct virtio_mmio_queue *live_queue(struct virtio_mmio *dev, uint32_t queue)
{
    const uint32_t ready = VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
    if (queue >= dev->backend->num_queues || !dev->queues[queue].ready ||
        (dev->status & ready) != ready ||
        (dev->status & (VIRTIO_STATUS_DEVICE_NEEDS_RESET | VIRTIO_STATUS_FAILED)))
        return NULL;
    return &dev->queues[queue];
}

/*
 * device 处理完一个队列之后：有新完成项且 driver 没关中断就拉中断线；
 * 处理函数报告队列已坏（rc < 0）就置 NEEDS_RESET。kick 路径和设备线程共用。
 */
static void queue_processed(struct virtio_mmio *dev, struct virtq *vq, uint16_t before, int rc)
{
    if (vq->next_used_idx != before) {
        /*
         * used.idx 的发布（store，在 virtq_used_push() 里）和读 avail.flags（load）
         * 之间需要全屏障：否则 load 可能提前到 store 之前，读到 driver 在看到新
         * used.idx 之前设置的 NO_INTERRUPT，漏发中断。
         */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        /*
         * driver 设置 NO_INTERRUPT 表示它正在轮询 used ring（例如 Linux 的
         * virtqueue_disable_cb()），不需要中断；这时只更新 ring，不拉中断线。
         */
        if (virtq_needs_interrupt(vq)) {
            dev->interrupt_status |= VIRTIO_MMIO_INT_VRING;
            update_irq(dev);
        }
    }
    if (rc < 0)
        needs_reset(dev);
}

int virtio_mmio_run_queue(struct virtio_mmio *dev, uint32_t queue,
                          int (*fn)(struct virtio_mmio *dev, struct virtq *vq, void *arg),
                          void *arg)
{
    pthread_mutex_lock(&dev->lock);
    struct virtio_mmio_queue *q = live_queue(dev, queue);
    if (q) {
        uint16_t before = q->vq.next_used_idx;
        int rc = fn(dev, &q->vq, arg);
        queue_processed(dev, &q->vq, before, rc);
    }
    pthread_mutex_unlock(&dev->lock);
    return q != NULL;
}

/*
 * driver 写 QueueNotify（doorbell）。这次 VM exit 发生在 vCPU 线程上，
 * 设备在这里同步处理完所有请求再返回客户机，所以 kick 返回时 used ring
 * 已经更新好了。
 *
 * 写入的值是队列号（不是请求数）。一次 kick 可能对应多个请求：driver 可以
 * 连续发布几条链再 kick 一次，所以 process_queue 要把 avail ring 处理到空。
 *
 * 同步处理是教学上的简化：真实 VMM 通常把 doorbell 注册成 ioeventfd，KVM 在内核里
 * 直接通知另一个线程（QEMU 的 iothread、Firecracker 的 VMM 线程、vhost 内核线程），
 * vCPU 不退出到用户态就回到客户机。这里磁盘读写期间 vCPU 被占住，客户机这段时间
 * 什么也做不了。
 */
static void queue_notify(struct virtio_mmio *dev, uint32_t queue)
{
    /* 队列号不存在、设备没准备好、已出错时，kick 一律忽略（见 live_queue()）。 */
    struct virtio_mmio_queue *q = live_queue(dev, queue);
    if (!q)
        return;
    dev->stats.notifies++;
    /* 记下处理前的完成计数，用来判断这次 kick 有没有完成任何请求。 */
    uint16_t before = q->vq.next_used_idx;
    int rc = dev->backend->process_queue(dev, queue, &q->vq);
    queue_processed(dev, &q->vq, before, rc);
}

/*
 * 写 Status 寄存器。driver 按 ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK
 * 的顺序逐步“加位”，每次写的是完整的新值（旧位 | 新位），不是只写新增的位。
 * 写 0 是复位。FAILED 表示 driver 放弃了这个设备。
 */
static void write_status(struct virtio_mmio *dev, uint32_t value)
{
    if (value == 0) {
        reset(dev);
        return;
    }
    uint32_t old = dev->status;
    /* driver 写不了 NEEDS_RESET：清不掉（只有复位能清），也伪造不出来。 */
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
    if ((next & VIRTIO_STATUS_DRIVER_OK) && !(old & VIRTIO_STATUS_DRIVER_OK) &&
        dev->backend->driver_ok)
        dev->backend->driver_ok(dev);
}

/*
 * 特性位有 64 位，但寄存器只有 32 位，所以用“选择器 + 窗口”的方式访问：
 * 先写 DriverFeaturesSel 选择第几个 32 位（0 = bit 0–31，1 = bit 32–63），
 * 再写 DriverFeatures 填这一段。DeviceFeatures 的读取方式相同。
 */
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

/*
 * 地址寄存器是 64 位值的低/高 32 位；只能在队列就绪之前写。
 * 就绪之后再改地址会让 device 手里的 vq 指针和 driver 以为的 ring 不一致，
 * 所以忽略；driver 要换 ring 必须先复位设备。
 */
static void write_queue_addr(struct virtio_mmio *dev, size_t field, int high, uint32_t value)
{
    struct virtio_mmio_queue *q = selected_queue(dev);
    if (!q || q->ready)
        return;
    uint64_t *addr = (uint64_t *)((uint8_t *)q + field);
    if (high)
        *addr = (*addr & 0xffffffffull) | (uint64_t)value << 32;
    else
        *addr = (*addr & ~0xffffffffull) | value;
}

/* 读一个 32 位寄存器。寄存器的读值大多由当前状态现算，没有单独存储。 */
static uint32_t read_reg(struct virtio_mmio *dev, uint64_t offset)
{
    const struct virtio_mmio_backend *be = dev->backend;
    switch (offset) {
    /* 前四个是设备身份，Linux probe 时先读它们判断“这里是不是 virtio 设备、是什么设备”。 */
    case VIRTIO_MMIO_MAGIC_VALUE:
        return VIRTIO_MMIO_MAGIC;
    case VIRTIO_MMIO_VERSION:
        return 2;
    case VIRTIO_MMIO_DEVICE_ID:
        return be->device_id;
    case VIRTIO_MMIO_VENDOR_ID:
        return VIRTIO_MMIO_VENDOR;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        /* 按 DeviceFeaturesSel 选择的窗口返回 64 位特性的一半。 */
        if (dev->device_features_sel > 1)
            return 0;
        return (uint32_t)(be->features >> (32 * dev->device_features_sel));
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        /* 0 表示“这个队列不存在”，driver 枚举队列时以此为止。 */
        return selected_queue(dev) ? be->queue_num_max : 0;
    case VIRTIO_MMIO_QUEUE_READY: {
        const struct virtio_mmio_queue *q = selected_queue(dev);
        return q ? (uint32_t)q->ready : 0;
    }
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        /* 中断处理函数读它判断中断原因：bit 0 有完成项，bit 1 配置/状态变化。 */
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
    case VIRTIO_MMIO_QUEUE_NUM: {
        struct virtio_mmio_queue *q = selected_queue(dev);
        if (q && !q->ready)
            q->num = value;
        return 0;
    }
    case VIRTIO_MMIO_QUEUE_READY:
        queue_set_ready(dev, value);
        return 0;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        queue_notify(dev, value);
        return 0;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        /*
         * driver 写回它读到的 InterruptStatus，表示“这些原因我处理了”。
         * 只清写入的位：ACK 之后新出现的原因不会被误清。
         */
        dev->interrupt_status &= ~value;
        update_irq(dev);
        return 0;
    case VIRTIO_MMIO_STATUS:
        write_status(dev, value);
        return 0;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        write_queue_addr(dev, offsetof(struct virtio_mmio_queue, desc),
                         offset == VIRTIO_MMIO_QUEUE_DESC_HIGH, value);
        return 0;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        write_queue_addr(dev, offsetof(struct virtio_mmio_queue, avail),
                         offset == VIRTIO_MMIO_QUEUE_AVAIL_HIGH, value);
        return 0;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        write_queue_addr(dev, offsetof(struct virtio_mmio_queue, used),
                         offset == VIRTIO_MMIO_QUEUE_USED_HIGH, value);
        return 0;
    case VIRTIO_MMIO_SHM_SEL:
        return 0;
    default:
        return -1;
    }
}

/*
 * 所有寄存器访问的入口，由 vcpu.c 在 KVM_EXIT_MMIO 时调用。
 * 一条客户机指令（例如 writel(v, base + 0x50)）对应一次调用：
 * offset = 0x50，len = 4，data 指向 kvm_run->mmio.data。读访问时我们写进
 * data 的值，会在下一次 KVM_RUN 时由 KVM 放进客户机指令的目标寄存器。
 */
static void access_locked(struct virtio_mmio *dev, uint64_t offset, uint8_t *data,
                          uint32_t len, int is_write)
{
    if (is_write)
        dev->stats.writes++;
    else
        dev->stats.reads++;

    /* 先挡住 VMM 自身可能出错的情况：宽度不对、越出这一页。 */
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
        /* memcpy 而不是强转指针：data 不保证 4 字节对齐。x86 和 virtio 都是小端。 */
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

void virtio_mmio_access(struct virtio_mmio *dev, uint64_t offset, uint8_t *data,
                        uint32_t len, int is_write)
{
    pthread_mutex_lock(&dev->lock);
    access_locked(dev, offset, data, len, is_write);
    pthread_mutex_unlock(&dev->lock);
}

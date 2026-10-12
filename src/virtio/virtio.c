#include "virtio/virtio.h"

#include <stdlib.h>

const char *virtio_device_name(uint32_t device_id)
{
    switch (device_id) {
    case VIRTIO_ID_NET:
        return "virtio-net";
    case VIRTIO_ID_BLOCK:
        return "virtio-blk";
    case VIRTIO_ID_CONSOLE:
        return "virtio-console";
    default:
        return NULL;
    }
}

struct virtio_device *virtio_device_create(uint32_t device_id, unsigned int qsz,
                                           void *mem, uint64_t mem_size)
{
    if (!virtio_device_name(device_id) || !mem)
        return NULL;
    struct virtio_device *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;
    dev->vq = virtq_create(qsz);
    if (!dev->vq) {
        free(dev);
        return NULL;
    }
    dev->device_id = device_id;
    dev->mem = mem;
    dev->mem_size = mem_size;
    return dev;
}

void virtio_device_destroy(struct virtio_device *dev)
{
    if (!dev)
        return;
    virtq_destroy(dev->vq);
    free(dev);
}

void virtio_device_set_ops(struct virtio_device *dev, const struct virtio_device_ops *ops,
                           void *opaque)
{
    dev->ops = ops;
    dev->opaque = opaque;
}

void *virtio_gpa(const struct virtio_device *dev, uint64_t gpa, uint64_t len)
{
    /* 写成减法，避免 gpa + len 溢出回绕后通过检查。 */
    if (gpa > dev->mem_size || len > dev->mem_size - gpa)
        return NULL;
    return dev->mem + gpa;
}

int virtio_device_kick(struct virtio_device *dev)
{
    const struct virtio_device_ops *ops = dev->ops;
    dev->notifications++;
    if (ops && ops->on_notify)
        ops->on_notify(dev);

    uint16_t used_before = dev->vq->next_used_idx;
    int rc = ops && ops->on_process ? ops->on_process(dev) : 0;
    uint16_t completed = (uint16_t)(dev->vq->next_used_idx - used_before);

    /* 即使 on_process 中途出错，已经发布的完成项也要通知 driver。 */
    if (completed && virtq_needs_interrupt(dev->vq)) {
        dev->interrupts++;
        if (ops && ops->on_interrupt)
            ops->on_interrupt(dev);
    }
    return rc < 0 ? -1 : completed;
}

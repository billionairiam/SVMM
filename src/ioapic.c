#include "ioapic.h"

#include <linux/kvm.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#define MASKED (UINT64_C(1) << 16)
#define LEVEL_TRIGGERED (UINT64_C(1) << 15)

int ioapic_init(struct ioapic *chip, int vm_fd)
{
    chip->vm_fd = vm_fd;
    chip->id = 1;
    for (unsigned i = 0; i < IOAPIC_PINS; ++i)
        chip->entries[i] = MASKED;
    return pthread_mutex_init(&chip->lock, NULL);
}

/* TDX has no EOI notification for a userspace IOAPIC. For the edge-only
 * platform, hold a pending edge while masked and inject once on unmask. */
static void deliver(struct ioapic *chip, unsigned pin)
{
    uint64_t entry = chip->entries[pin];
    uint32_t bit = 1u << pin;
    if (!(chip->pending & bit) || (entry & (MASKED | LEVEL_TRIGGERED)))
        return;
    unsigned vector = entry & 0xff;
    unsigned mode = (entry >> 8) & 7;
    if (vector < 16 || mode > 1)
        return;

    struct kvm_msi irq = {
        .address_lo = 0xfee00000u | ((entry >> 56) << 12) |
                      ((entry & (1u << 11)) ? 4u : 0u),
        .data = vector | (mode << 8),
    };
    int result = ioctl(chip->vm_fd, KVM_SIGNAL_MSI, &irq);
    if (result > 0)
        chip->pending &= ~bit;
    else if (result < 0)
        perror("KVM_SIGNAL_MSI");
}

void ioapic_signal(struct ioapic *chip, unsigned pin, int level)
{
    if (pin >= IOAPIC_PINS)
        return;
    const uint32_t bit = 1u << pin;
    pthread_mutex_lock(&chip->lock);
    if (level) {
        if (!(chip->high & bit))
            chip->pending |= bit;
        chip->high |= bit;
        deliver(chip, pin);
    } else {
        chip->high &= ~bit;
    }
    pthread_mutex_unlock(&chip->lock);
}

void ioapic_access(void *opaque, uint64_t offset, uint8_t *data, uint32_t len, int write)
{
    struct ioapic *chip = opaque;
    if (len != 4 || (offset != 0 && offset != 0x10)) {
        if (!write)
            memset(data, 0, len);
        return;
    }
    uint32_t value = 0;
    if (write)
        memcpy(&value, data, sizeof(value));
    pthread_mutex_lock(&chip->lock);
    if (!offset) {
        if (write)
            chip->selector = value & 0xff;
        else
            value = chip->selector;
    } else if (chip->selector == 0) {
        if (write)
            chip->id = (value >> 24) & 0xf;
        else
            value = chip->id << 24;
    } else if (chip->selector == 1) {
        value = ((IOAPIC_PINS - 1u) << 16) | 0x11u;
    } else if (chip->selector == 2) {
        value = chip->id << 24;
    } else if (chip->selector >= 0x10 && chip->selector < 0x10 + 2 * IOAPIC_PINS) {
        unsigned pin = (chip->selector - 0x10) / 2;
        unsigned shift = (chip->selector & 1) ? 32 : 0;
        if (write) {
            uint64_t mask = (uint64_t)(shift ? 0xff000000u : 0x0001afffu) << shift;
            chip->entries[pin] = (chip->entries[pin] & ~mask) | (((uint64_t)value << shift) & mask);
            deliver(chip, pin);
        } else {
            value = (uint32_t)(chip->entries[pin] >> shift);
        }
    }
    pthread_mutex_unlock(&chip->lock);
    if (!write)
        memcpy(data, &value, sizeof(value));
}

void ioapic_destroy(struct ioapic *chip)
{
    pthread_mutex_destroy(&chip->lock);
}

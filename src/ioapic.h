#ifndef SVMM_IOAPIC_H
#define SVMM_IOAPIC_H

#include <pthread.h>
#include <stdint.h>

#define IOAPIC_PINS 24u

struct ioapic {
    pthread_mutex_t lock;
    int vm_fd;
    uint32_t selector;
    uint32_t id;
    uint32_t high, pending;
    uint64_t entries[IOAPIC_PINS];
};

int ioapic_init(struct ioapic *chip, int vm_fd);
void ioapic_signal(struct ioapic *chip, unsigned pin, int level);
void ioapic_access(void *opaque, uint64_t offset, uint8_t *data, uint32_t len, int write);
void ioapic_destroy(struct ioapic *chip);

#endif

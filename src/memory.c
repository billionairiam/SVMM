#include "memory.h"

#include <errno.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

int guest_memory_init(struct guest_memory *memory, int vm_fd, size_t size)
{
    memory->data = NULL;
    memory->size = 0;
    if (size < GUEST_MEMORY_MIN_SIZE || (size & 0xfff) != 0) {
        errno = EINVAL;
        return -1;
    }
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap guest memory");
        return -1;
    }
    memory->data = mapping;
    memory->size = size;

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = memory->size,
        .userspace_addr = (uint64_t)(uintptr_t)memory->data,
    };
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        guest_memory_destroy(memory);
        return -1;
    }
    return 0;
}

int guest_memory_load(struct guest_memory *memory, size_t address,
                      const void *source, size_t size)
{
    if (address > memory->size || size > memory->size - address) {
        errno = EINVAL;
        return -1;
    }
    memcpy(memory->data + address, source, size);
    return 0;
}

void guest_memory_destroy(struct guest_memory *memory)
{
    if (memory->data)
        munmap(memory->data, memory->size);
    memory->data = NULL;
    memory->size = 0;
}

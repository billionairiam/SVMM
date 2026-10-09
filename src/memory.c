#include "memory.h"

#include <errno.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/*
 * 分配 size 字节的客户机 RAM，并注册为 GPA [0, size)。
 *
 * KVM 本身不分配客户机内存：VMM 先在自己的进程里 mmap 一块匿名内存，
 * 再用 KVM_SET_USER_MEMORY_REGION 告诉 KVM“客户机物理地址 0 开始的这段
 * 就是这块宿主机内存”。之后客户机读写 RAM 不会产生 VM exit，VMM 也可以
 * 直接通过 memory->data 读写客户机内存（加载内核就是这样做的）。
 * size 必须是 4 KiB 页的整数倍。成功返回 0，失败返回 -1。
 */
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

    /*
     * 匿名映射的页在第一次访问时才真正分配，所以 128 MiB 的客户机
     * 并不会立刻占用 128 MiB 宿主机内存。slot 是 KVM 中内存区域的编号，
     * 本 VMM 只有一个区域。
     */
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

/*
 * 把宿主机缓冲区复制到客户机物理地址 address。这就是 VMM 的“加载器”：
 * 内核、setup 副本、命令行、boot_params 和 GDT 都通过它进入客户机内存。
 * 越界时返回 -1 且 errno = EINVAL；用减法比较避免 address + size 溢出。
 */
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

/* 解除映射并恢复为空状态，可对未初始化或已释放的对象重复调用。 */
void guest_memory_destroy(struct guest_memory *memory)
{
    if (memory->data)
        munmap(memory->data, memory->size);
    memory->data = NULL;
    memory->size = 0;
}

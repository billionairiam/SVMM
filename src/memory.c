#include "memory.h"

#include <errno.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

/*
 * 客户机“物理内存”其实就是 VMM 进程里的一段普通虚拟内存。
 *
 * 地址有三种，容易混：
 *   GVA  客户机虚拟地址：客户机进程/内核用的地址，经客户机页表变成 GPA
 *   GPA  客户机物理地址：客户机以为的物理地址，e820、描述符里的 addr 都是 GPA
 *   HVA  宿主虚拟地址：VMM 进程里的指针，就是这里 mmap 返回的地址
 *
 * KVM_SET_USER_MEMORY_REGION 登记一个“内存槽”：GPA [guest_phys_addr,
 * guest_phys_addr + memory_size) ↔ HVA [userspace_addr, ...)。之后：
 *   - 客户机访问这段 GPA：硬件（Intel EPT / AMD NPT）把 GPA 翻译成宿主物理页，
 *     不产生 VM exit，速度和普通内存访问一样；
 *   - VMM 访问客户机内存：GPA 换算成 data + gpa，直接读写指针即可，
 *     加载内核、写 ACPI 表、virtio 设备读写 ring 和 buffer 都是这么做的；
 *   - 客户机访问不在任何内存槽里的 GPA：产生 KVM_EXIT_MMIO，交给 VMM 模拟。
 *     virtio-mmio 的寄存器页（0xd0000000）就是靠“故意不映射”来拦截访问的。
 *
 * 本项目只有一个槽（slot 0），GPA 从 0 开始，所以 GPA 和偏移相同。
 */
/* fd < 0：匿名内存（boot）；fd >= 0：把快照的内存文件私有映射进来（Stage 11 恢复）。 */
static int map_guest_memory(struct guest_memory *memory, int vm_fd, size_t size_mb, int fd)
{
    memory->data = NULL;
    memory->size = 0;
    if (size_mb < GUEST_MEMORY_MIN_MB || size_mb > GUEST_MEMORY_MAX_MB) {
        fprintf(stderr, "guest memory must be %u-%u MiB, got %zu MiB\n",
                GUEST_MEMORY_MIN_MB, GUEST_MEMORY_MAX_MB, size_mb);
        errno = EINVAL;
        return -1;
    }
    size_t size = size_mb * MIB;
    /*
     * 匿名映射：内核只保留地址范围，页面在第一次被访问时才分配并清零，
     * 所以 --mem 3072 也不会立刻占用 3 GiB 宿主内存。MAP_PRIVATE 即可，
     * 这块内存只在本进程内与 KVM 共享，不需要和其他进程共享。
     *
     * 文件映射（热快照恢复）同样是 MAP_PRIVATE：缺页时从文件（页缓存）读，客户机
     * 写入时复制一份匿名页，永远不会写回快照文件。这就是 Firecracker 的
     * “懒恢复”：恢复时不用把整块内存读一遍，客户机碰到哪页才读哪页。
     */
    void *mapping = fd < 0 ? mmap(NULL, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
                           : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap guest memory");
        return -1;
    }
    memory->data = mapping;
    memory->size = size;

    /*
     * flags 为 0：可读写、不记录脏页（热迁移才需要 KVM_MEM_LOG_DIRTY_PAGES）。
     * 同一个 slot 再调用一次可以修改或删除（memory_size = 0）这个映射。
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

int guest_memory_init(struct guest_memory *memory, int vm_fd, size_t size_mb)
{
    return map_guest_memory(memory, vm_fd, size_mb, -1);
}

int guest_memory_init_file(struct guest_memory *memory, int vm_fd, size_t size_mb, int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != size_mb * MIB) {
        fprintf(stderr, "guest memory file must be a regular file of %zu MiB\n", size_mb);
        errno = EINVAL;
        return -1;
    }
    return map_guest_memory(memory, vm_fd, size_mb, fd);
}

/*
 * 把宿主数据复制到客户机物理地址 address 处（启动前加载内核、initramfs、
 * ACPI 表用）。用减法检查范围，address + size 溢出时也不会写出界。
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

void guest_memory_destroy(struct guest_memory *memory)
{
    if (memory->data)
        munmap(memory->data, memory->size);
    memory->data = NULL;
    memory->size = 0;
}

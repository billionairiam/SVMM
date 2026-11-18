#include "tdx.h"

#include "kvm.h"
#include "memory.h"
#include "vcpu.h"

#include <asm/kvm_para.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef KVM_X86_TDX_VM
#define KVM_X86_TDX_VM 5
#endif

/* Linux 7.0 x86 KVM UAPI. Local definitions let older userspace headers build. */
enum td_command {
    TD_CAPABILITIES, TD_INIT_VM, TD_INIT_VCPU, TD_INIT_MEMORY, TD_FINALIZE, TD_GET_CPUID,
};
#define TD_SEPT_VE_DISABLE (UINT64_C(1) << 28)
#define TD_XFAM_SSE UINT64_C(3) /* x87 and SSE */
#define TD_XFAM_AVX (UINT64_C(1) << 2)
#define CPUID_AVX (1u << 28)
#define CPUID_F16C (1u << 29)
#define TD_CPUID_MAX 256u
#define TD_MAP_GPA 12u
#define TD_MAP_PRIVATE 0x10u

struct td_ioctl {
    uint32_t command, flags;
    uint64_t data, hw_error;
};

struct td_cpuid {
    uint32_t count, padding;
    struct kvm_cpuid_entry2 entries[TD_CPUID_MAX];
};

struct td_capabilities {
    uint64_t attrs, xfam, reserved[254];
    struct td_cpuid cpuid;
};

struct td_parameters {
    uint64_t attrs, xfam, reserved[30];
    struct td_cpuid cpuid;
};

struct td_import {
    uint64_t address, gpa, pages;
};

static int td_call(int fd, enum td_command op, uint32_t flags, uint64_t arg)
{
    static const char *const names[] = {
        "CAPABILITIES", "INIT_VM", "INIT_VCPU", "INIT_MEM_REGION", "FINALIZE_VM", "GET_CPUID",
    };
    struct td_ioctl request = { .command = op, .flags = flags, .data = arg };
    int ret = ioctl(fd, KVM_MEMORY_ENCRYPT_OP, &request);
    if (ret < 0 && errno != EINTR)
        fprintf(stderr, "ERROR: KVM_TDX_%s: %s (hardware status %#llx)\n",
                names[op], strerror(errno), (unsigned long long)request.hw_error);
    return ret;
}

static int check_cap(int fd, int cap, unsigned mask, const char *name)
{
    int result = ioctl(fd, KVM_CHECK_EXTENSION, cap);
    if (result < 0 || ((unsigned)result & mask) != mask) {
        fprintf(stderr, "ERROR: KVM TDX requires %s\n", name);
        return -1;
    }
    return 0;
}

static int open_tdx_kvm(void)
{
    int fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/kvm for TDX");
        return -1;
    }
    int version = ioctl(fd, KVM_GET_API_VERSION, 0);
    if (version != KVM_API_VERSION) {
        fprintf(stderr, "ERROR: KVM API version %d unavailable on /dev/kvm\n", KVM_API_VERSION);
        close(fd);
        return -1;
    }
    if (check_cap(fd, KVM_CAP_VM_TYPES, 1u << KVM_X86_TDX_VM, "KVM_X86_TDX_VM") ||
        check_cap(fd, KVM_CAP_MEMORY_ATTRIBUTES, KVM_MEMORY_ATTRIBUTE_PRIVATE, "private memory attributes") ||
        check_cap(fd, KVM_CAP_GUEST_MEMFD, 1, "guest_memfd") ||
        check_cap(fd, KVM_CAP_SPLIT_IRQCHIP, 1, "split irqchip") ||
        check_cap(fd, KVM_CAP_SIGNAL_MSI, 1, "MSI injection") ||
        check_cap(fd, KVM_CAP_EXIT_HYPERCALL, 1u << TD_MAP_GPA, "MapGPA exits")) {
        close(fd);
        return -1;
    }
    return fd;
}

static int create_td(int fd)
{
    int vm = ioctl(fd, KVM_CREATE_VM, (unsigned long)KVM_X86_TDX_VM);
    if (vm < 0)
        perror("KVM_CREATE_VM(TDX)");
    return vm;
}

static int check_td_capabilities(int vm, struct tdx_vm *td)
{
    struct td_capabilities caps = { .cpuid.count = TD_CPUID_MAX };
    if (td_call(vm, TD_CAPABILITIES, 0, (uintptr_t)&caps) < 0)
        return -1;
    if (!(caps.attrs & TD_SEPT_VE_DISABLE) || (caps.xfam & TD_XFAM_SSE) != TD_XFAM_SSE) {
        fprintf(stderr, "ERROR: TDX host needs SEPT #VE disable and x87/SSE state\n");
        return -1;
    }
    if (td) {
        td->xfam = caps.xfam & (TD_XFAM_SSE | TD_XFAM_AVX);
        if (td->xfam & TD_XFAM_AVX) {
            for (uint32_t i = 0; i < caps.cpuid.count; ++i) {
                const struct kvm_cpuid_entry2 *entry = &caps.cpuid.entries[i];
                if (entry->function == 1)
                    td->cpuid_avx = entry->ecx & (CPUID_AVX | CPUID_F16C);
            }
        }
    }
    return 0;
}

int tdx_preflight(const char *firmware)
{
    struct td_shim *shim = td_shim_open(firmware);
    if (!shim)
        return -1;
    td_shim_close(shim);
    int fd = open_tdx_kvm();
    if (fd < 0)
        return -1;
    int vm = create_td(fd);
    int result = -1;
    if (vm >= 0) {
        result = check_td_capabilities(vm, NULL);
        close(vm);
    }
    close(fd);
    return result;
}

static int memory_attributes(int fd, uint64_t gpa, uint64_t size, int priv)
{
    struct kvm_memory_attributes attributes = {
        .address = gpa, .size = size,
        .attributes = priv ? KVM_MEMORY_ATTRIBUTE_PRIVATE : 0,
    };
    return ioctl(fd, KVM_SET_MEMORY_ATTRIBUTES, &attributes);
}

static int private_slot(int vm, unsigned slot, uint64_t gpa, uint64_t size,
                        uint8_t *data)
{
    struct kvm_create_guest_memfd create = { .size = size };
    int fd = ioctl(vm, KVM_CREATE_GUEST_MEMFD, &create);
    if (fd < 0) {
        perror("KVM_CREATE_GUEST_MEMFD");
        return -1;
    }
    struct kvm_userspace_memory_region2 region = {
        .slot = slot, .flags = KVM_MEM_GUEST_MEMFD, .guest_phys_addr = gpa,
        .memory_size = size, .userspace_addr = (uintptr_t)data, .guest_memfd = fd,
    };
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION2, &region) < 0 ||
        memory_attributes(vm, gpa, size, 1) < 0) {
        perror("register TDX private memory");
        close(fd);
        return -1;
    }
    return fd;
}

static int enable_cap(int fd, unsigned name, uint64_t value)
{
    struct kvm_enable_cap cap = { .cap = name, .args = { value } };
    if (ioctl(fd, KVM_ENABLE_CAP, &cap) < 0) {
        fprintf(stderr, "ERROR: enable TDX KVM capability %u: %s\n", name, strerror(errno));
        return -1;
    }
    return 0;
}

int tdx_vm_open(struct tdx_vm *td, struct kvm_context *kvm, struct guest_memory *memory,
                size_t ram_size)
{
    kvm->system_fd = open_tdx_kvm();
    if (kvm->system_fd < 0)
        return -1;
    kvm->vm_fd = create_td(kvm->system_fd);
    if (kvm->vm_fd < 0 || check_td_capabilities(kvm->vm_fd, td) < 0)
        return -1;
    td->vm_fd = kvm->vm_fd;
    if (enable_cap(td->vm_fd, KVM_CAP_SPLIT_IRQCHIP, 24) ||
        enable_cap(td->vm_fd, KVM_CAP_EXIT_HYPERCALL, 1u << TD_MAP_GPA))
        return -1;

    int error = pthread_mutex_init(&td->dma_lock, NULL);
    if (error) {
        fprintf(stderr, "ERROR: initialize TDX DMA lock: %s\n", strerror(error));
        return -1;
    }
    td->dma_lock_ready = 1;

    td->ram = memory;
    uint64_t low_limit = (uint64_t)GUEST_MEMORY_MAX_MB * MIB;
    td->low_ram_size = ram_size < low_limit ? ram_size : low_limit;
    td->high_ram_size = ram_size - td->low_ram_size;
    uint64_t map_size = td->high_ram_size ? TDX_HIGH_RAM_BASE + td->high_ram_size : ram_size;
    void *mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("allocate TDX RAM backing");
        return -1;
    }
    memory->data = mapping;
    memory->size = map_size;
    td->shared_pages = calloc(map_size / TDX_PAGE_SIZE, 1);
    if (!td->shared_pages) {
        perror("allocate TDX shared page map");
        return -1;
    }
    td->ram_fd = private_slot(td->vm_fd, 0, 0, td->low_ram_size, memory->data);
    if (td->ram_fd < 0)
        return -1;
    if (td->high_ram_size) {
        td->high_ram_fd = private_slot(td->vm_fd, 2, TDX_HIGH_RAM_BASE,
                                      td->high_ram_size, memory->data + TDX_HIGH_RAM_BASE);
        if (td->high_ram_fd < 0)
            return -1;
    }
    return 0;
}

static int launch_cpu(struct tdx_vm *td, struct kvm_context *kvm,
                      struct vcpu *vcpu, uint64_t hob)
{
    /* Select four-level EPT. AVX needs both xstate and any configurable
     * instruction bits advertised by the TDX capabilities. */
    struct td_parameters params = {
        .attrs = TD_SEPT_VE_DISABLE, .xfam = td->xfam,
        .cpuid = { .count = 1, .entries = { { .function = 0x80000008, .eax = 48u << 16 } } },
    };
    if (td->cpuid_avx)
        params.cpuid.entries[params.cpuid.count++] = (struct kvm_cpuid_entry2){
            .function = 1, .ecx = td->cpuid_avx,
        };
    if (td_call(td->vm_fd, TD_INIT_VM, 0, (uintptr_t)&params) < 0 ||
        vcpu_open(vcpu, kvm, 0) < 0)
        return -1;

    /* KVM needs x2APIC to initialize the vCPU. After initialization use
     * the module's architectural CPUID values. */
    struct td_cpuid cpuid = {
        .count = 1, .entries = { { .function = 1, .ecx = 1u << 21 } },
    };
    if (ioctl(vcpu->fd, KVM_SET_CPUID2, &cpuid) < 0) {
        perror("enable x2APIC CPUID for TDX");
        return -1;
    }
    cpuid.count = TD_CPUID_MAX;
    if (td_call(vcpu->fd, TD_INIT_VCPU, 0, hob) < 0 ||
        td_call(vcpu->fd, TD_GET_CPUID, 0, (uintptr_t)&cpuid) < 0)
        return -1;
    if (cpuid.count > TD_CPUID_MAX - 2) {
        fprintf(stderr, "ERROR: TDX CPUID table has no room for KVM leaves\n");
        return -1;
    }
    /* Linux uses the KVM signature to enable x2APIC without IRQ remapping.
     * Leave the feature leaf zero, so Linux uses the native TD clock and
     * interrupts instead of KVM's ordinary paravirtual MSRs. */
    cpuid.entries[cpuid.count++] = (struct kvm_cpuid_entry2){
        .function = KVM_CPUID_SIGNATURE, .eax = KVM_CPUID_FEATURES,
        .ebx = 0x4b4d564b, .ecx = 0x564b4d56, .edx = 0x4d, /* KVMKVMKVM */
    };
    cpuid.entries[cpuid.count++] = (struct kvm_cpuid_entry2){ .function = KVM_CPUID_FEATURES };
    if (ioctl(vcpu->fd, KVM_SET_CPUID2, &cpuid) < 0) {
        perror("KVM_SET_CPUID2 for TDX");
        return -1;
    }
    return 0;
}

static int import_pages(int fd, uint8_t *data, uint64_t start, uint64_t size, int measure)
{
    if (!size)
        return 0;
    struct td_import range = {
        .address = (uintptr_t)data, .gpa = start, .pages = size / TDX_PAGE_SIZE,
    };
    for (;;) {
        if (td_call(fd, TD_INIT_MEMORY, measure, (uintptr_t)&range) == 0)
            return 0;
        if (errno != EINTR)
            return -1;
        if (range.pages == 0)
            return 0;
    }
}

int tdx_boot(struct tdx_vm *td, struct kvm_context *kvm, struct vcpu *vcpu,
             const struct td_shim *shim, const struct linux_image *image,
             const struct linux_initrd *initrd, const char *cmdline)
{
    struct tdx_range ranges[3] = { 0 };
    uint64_t hob;
    if (td_shim_stage(shim, td, image, initrd, cmdline, &hob, ranges) < 0 ||
        launch_cpu(td, kvm, vcpu, hob) < 0)
        return -1;

    td->firmware_fd = private_slot(td->vm_fd, 1, td->firmware_base,
                                  td->firmware_size, td->firmware);
    if (td->firmware_fd < 0)
        return -1;
    uint64_t cursor = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (!ranges[i].size)
            continue;
        if (ranges[i].start < cursor || ranges[i].start > td->low_ram_size ||
            ranges[i].size > td->low_ram_size - ranges[i].start)
            return -1;
        if (import_pages(vcpu->fd, td->ram->data + cursor, cursor, ranges[i].start - cursor, 0) ||
            import_pages(vcpu->fd, td->ram->data + ranges[i].start,
                         ranges[i].start, ranges[i].size, 1))
            return -1;
        cursor = ranges[i].start + ranges[i].size;
    }
    if (import_pages(vcpu->fd, td->ram->data + cursor, cursor, td->low_ram_size - cursor, 0) ||
        (td->high_ram_size && import_pages(vcpu->fd, td->ram->data + TDX_HIGH_RAM_BASE,
                                           TDX_HIGH_RAM_BASE, td->high_ram_size, 0)) ||
        import_pages(vcpu->fd, td->firmware, td->firmware_base, td->firmware_size, 1) ||
        td_call(td->vm_fd, TD_FINALIZE, 0, 0) < 0)
        return -1;
    if (madvise(td->ram->data, td->ram->size, MADV_DONTNEED) < 0) {
        perror("clear TDX staging memory");
        return -1;
    }
    vcpu->tdx = 1;
    vcpu->hypercall = tdx_handle_hypercall;
    vcpu->hypercall_opaque = td;
    fprintf(stderr, "INFO: TDX launch finalized (debug disabled)\n");
    return 0;
}

static int ram_fd_for_range(const struct tdx_vm *td, uint64_t start, uint64_t size)
{
    if (start < td->low_ram_size && size <= td->low_ram_size - start)
        return td->ram_fd;
    if (start >= TDX_HIGH_RAM_BASE && start - TDX_HIGH_RAM_BASE < td->high_ram_size &&
        size <= td->high_ram_size - (start - TDX_HIGH_RAM_BASE))
        return td->high_ram_fd;
    return -1;
}

int tdx_handle_hypercall(void *opaque, struct kvm_run *run)
{
    struct tdx_vm *td = opaque;
    if (run->hypercall.nr != TD_MAP_GPA)
        return -1;
    uint64_t start = run->hypercall.args[0], pages = run->hypercall.args[1];
    uint64_t mode = run->hypercall.args[2];
    if ((start % TDX_PAGE_SIZE) || start >= td->ram->size || !pages ||
        pages > (td->ram->size - start) / TDX_PAGE_SIZE ||
        (mode != 0 && mode != TD_MAP_PRIVATE)) {
        run->hypercall.ret = -EINVAL;
        return 0;
    }
    uint64_t size = pages * TDX_PAGE_SIZE;
    int fd = ram_fd_for_range(td, start, size);
    if (fd < 0) {
        run->hypercall.ret = -EINVAL;
        return 0;
    }
    uint64_t offset = fd == td->high_ram_fd ? start - TDX_HIGH_RAM_BASE : start;
    int priv = mode == TD_MAP_PRIVATE;
    pthread_mutex_lock(&td->dma_lock);
    /* Device threads must stop using these pages even if changing one of
     * the backings fails after KVM has changed the memory attribute. */
    memset(td->shared_pages + start / TDX_PAGE_SIZE, 0, pages);
    if (memory_attributes(td->vm_fd, start, size, priv) < 0 ||
        (priv ? madvise(td->ram->data + start, size, MADV_DONTNEED) :
         fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, size)) < 0) {
        perror("convert TDX backing");
        pthread_mutex_unlock(&td->dma_lock);
        return -1; /* No device access after a partial transition. */
    }
    memset(td->shared_pages + start / TDX_PAGE_SIZE, !priv, pages);
    pthread_mutex_unlock(&td->dma_lock);
    run->hypercall.ret = 0;
    return 0;
}

void *tdx_dma(void *opaque, uint64_t address, uint64_t length)
{
    const struct tdx_vm *td = opaque;
    uint64_t start = address & ~TDX_GPA_SHARED;
    if (ram_fd_for_range(td, start, length) < 0)
        return NULL;
    size_t first = start / TDX_PAGE_SIZE;
    size_t last = (start + (length ? length - 1 : 0)) / TDX_PAGE_SIZE;
    for (size_t page = first; page <= last; ++page) {
        if (!td->shared_pages[page])
            return NULL;
    }
    return td->ram->data + start;
}

void tdx_vm_close(struct tdx_vm *td)
{
    if (td->dma_lock_ready)
        pthread_mutex_destroy(&td->dma_lock);
    if (td->firmware)
        munmap(td->firmware, td->firmware_size);
    if (td->firmware_fd >= 0)
        close(td->firmware_fd);
    if (td->ram_fd >= 0)
        close(td->ram_fd);
    if (td->high_ram_fd >= 0)
        close(td->high_ram_fd);
    free(td->shared_pages);
}

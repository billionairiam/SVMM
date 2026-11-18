#ifndef SVMM_TDX_H
#define SVMM_TDX_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct kvm_context;
struct kvm_run;
struct vcpu;
struct guest_memory;
struct sandbox_config;
struct linux_image;
struct linux_initrd;

#define TDX_PAGE_SIZE 4096u
#define TDX_GPA_SHARED (UINT64_C(1) << 47)
#define TDX_HIGH_RAM_BASE (UINT64_C(1) << 32)
#define TDX_MAX_MEM_MB (32u * 1024u)
#define TDX_MAX_VCPUS 64u

struct tdx_range {
    uint64_t start, size;
};

struct tdx_vm {
    int vm_fd, ram_fd, high_ram_fd, firmware_fd;
    unsigned vcpus;
    struct guest_memory *ram;
    uint64_t low_ram_size, high_ram_size;
    uint8_t *firmware;
    uint64_t firmware_base, firmware_size;
    uint8_t *shared_pages;
    pthread_mutex_t dma_lock;
    int dma_lock_ready;
    uint64_t xfam;
    uint32_t cpuid_avx;
};

/* Parsed, bounded boot material. No pointers to KVM private pages. */
struct td_shim;
struct td_shim *td_shim_open(const char *path);
void td_shim_close(struct td_shim *shim);
int td_shim_stage(const struct td_shim *shim, struct tdx_vm *td,
                  const struct linux_image *image, const struct linux_initrd *initrd,
                  const char *cmdline, uint64_t *hob, struct tdx_range measure[3]);

int tdx_preflight(const char *firmware, unsigned vcpus);
int tdx_vm_open(struct tdx_vm *td, struct kvm_context *kvm, struct guest_memory *memory,
                size_t ram_size, unsigned vcpus);
int tdx_boot(struct tdx_vm *td, struct kvm_context *kvm, struct vcpu *vcpu,
             const struct td_shim *shim, const struct linux_image *image,
             const struct linux_initrd *initrd, const char *cmdline);
int tdx_handle_hypercall(void *opaque, struct kvm_run *run);
void *tdx_dma(void *opaque, uint64_t address, uint64_t length);
void tdx_vm_close(struct tdx_vm *td);

#endif

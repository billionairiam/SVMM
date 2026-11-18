#ifndef SVMM_TDX_H
#define SVMM_TDX_H

#include <stddef.h>
#include <stdint.h>

struct kvm_context;
struct kvm_run;
struct vcpu;
struct guest_memory;
struct sandbox_config;
struct linux_image;
struct linux_initrd;

#define TDX_PAGE_SIZE 4096u
#define TDX_GPA_SHARED (UINT64_C(1) << 47)

struct tdx_range {
    uint64_t start, size;
};

struct tdx_vm {
    int vm_fd, ram_fd, firmware_fd;
    struct guest_memory *ram;
    uint8_t *firmware;
    uint64_t firmware_base, firmware_size;
    uint8_t *shared_pages;
};

/* Parsed, bounded boot material. No pointers to KVM private pages. */
struct td_shim;
struct td_shim *td_shim_open(const char *path);
void td_shim_close(struct td_shim *shim);
int td_shim_stage(const struct td_shim *shim, struct tdx_vm *td,
                  const struct linux_image *image, const struct linux_initrd *initrd,
                  const char *cmdline, uint64_t *hob, struct tdx_range measure[3]);

int tdx_preflight(const char *firmware);
int tdx_vm_open(struct tdx_vm *td, struct kvm_context *kvm, struct guest_memory *memory,
                size_t ram_size);
int tdx_boot(struct tdx_vm *td, struct kvm_context *kvm, struct vcpu *vcpu,
             const struct td_shim *shim, const struct linux_image *image,
             const struct linux_initrd *initrd, const char *cmdline);
int tdx_handle_hypercall(void *opaque, struct kvm_run *run);
void *tdx_dma(void *opaque, uint64_t address, uint64_t length);
void tdx_vm_close(struct tdx_vm *td);

#endif

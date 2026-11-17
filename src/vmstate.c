#include "vmstate.h"

#include "kvm.h"
#include "vcpu.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

/*
 * 读写 KVM 状态时的顺序（参考 Firecracker / QEMU）：
 *
 *   保存   mp_state → regs → sregs → xsave → xcrs → debugregs → lapic → msrs → events
 *   恢复   同样的顺序，其中几条约束是硬性的：
 *          - sregs 在 msrs 之前：EFER 等在 sregs 里，部分 MSR 的合法性取决于它；
 *          - lapic 在 msrs 之前：MSR_IA32_TSC_DEADLINE 只有 LAPIC 处在 TSC-deadline
 *            模式时写入才生效，否则被 KVM 丢掉，客户机的下一次定时器中断就没了；
 *          - events 最后：它带着挂起的异常/中断，前面的 SET 可能把它清掉。
 *
 * VM 级状态（中断控制器、PIT、kvmclock）在 vCPU 之前恢复，和真机上电顺序一致。
 */

#define MSR_INDEX_LIST_MAX 1024u

static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
    if (ioctl(fd, req, arg) < 0) {
        fprintf(stderr, "ERROR: vmstate: %s: %s\n", what, strerror(errno));
        return -1;
    }
    return 0;
}

static int get_cpuid(int vcpu_fd, struct vmstate_cpu *cpu)
{
    size_t bytes = sizeof(struct kvm_cpuid2) + VMSTATE_MAX_CPUID * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid2 *c = calloc(1, bytes);
    if (!c) {
        fprintf(stderr, "ERROR: vmstate: out of memory\n");
        return -1;
    }
    c->nent = VMSTATE_MAX_CPUID;
    int ret = xioctl(vcpu_fd, KVM_GET_CPUID2, c, "KVM_GET_CPUID2");
    if (ret == 0) {
        cpu->ncpuid = c->nent;
        memcpy(cpu->cpuid, c->entries, c->nent * sizeof(c->entries[0]));
    }
    free(c);
    return ret;
}

static int get_tsc_khz(int vcpu_fd, uint32_t *khz)
{
    int r = ioctl(vcpu_fd, KVM_GET_TSC_KHZ, 0);
    if (r <= 0) {
        fprintf(stderr, "ERROR: vmstate: KVM_GET_TSC_KHZ: %s\n", r < 0 ? strerror(errno) : "0");
        return -1;
    }
    *khz = (uint32_t)r;
    return 0;
}

/*
 * KVM_GET_MSR_INDEX_LIST 列出的是 KVM 认识的全部 MSR，但其中一些在这颗 CPU / 这份
 * CPUID 下读不出来（例如没有对应特性）。KVM_GET_MSRS 遇到第一个读不出来的就停下并
 * 返回已读的个数，所以跳过那一个继续读，最后留下的都是能读出来的。
 */
static int get_msrs(const struct kvm_context *kvm, int vcpu_fd, struct vmstate_cpu *cpu)
{
    struct kvm_msr_list *list = calloc(1, sizeof(*list) + MSR_INDEX_LIST_MAX * sizeof(uint32_t));
    struct kvm_msrs *msrs = calloc(1, sizeof(*msrs) + MSR_INDEX_LIST_MAX * sizeof(msrs->entries[0]));
    int ret = -1;
    if (!list || !msrs) {
        fprintf(stderr, "ERROR: vmstate: out of memory\n");
        goto out;
    }
    list->nmsrs = MSR_INDEX_LIST_MAX;
    if (xioctl(kvm->system_fd, KVM_GET_MSR_INDEX_LIST, list, "KVM_GET_MSR_INDEX_LIST") < 0)
        goto out;
    cpu->nmsrs = 0;
    uint32_t next = 0;
    while (next < list->nmsrs) {
        uint32_t n = list->nmsrs - next;
        msrs->nmsrs = n;
        for (uint32_t i = 0; i < n; i++)
            msrs->entries[i] = (struct kvm_msr_entry){ .index = list->indices[next + i] };
        int got = ioctl(vcpu_fd, KVM_GET_MSRS, msrs);
        if (got < 0) {
            fprintf(stderr, "ERROR: vmstate: KVM_GET_MSRS: %s\n", strerror(errno));
            goto out;
        }
        if (cpu->nmsrs + (uint32_t)got > VMSTATE_MAX_MSRS) {
            fprintf(stderr, "ERROR: vmstate: more than %u readable MSRs\n", VMSTATE_MAX_MSRS);
            goto out;
        }
        memcpy(&cpu->msrs[cpu->nmsrs], msrs->entries, (size_t)got * sizeof(msrs->entries[0]));
        cpu->nmsrs += (uint32_t)got;
        next += (uint32_t)got + 1; /* 跳过读不出来的那一个 */
    }
    ret = 0;
out:
    free(list);
    free(msrs);
    return ret;
}

int vmstate_capture(struct vmstate *st, const struct kvm_context *kvm, const struct vcpu *vcpu)
{
    struct vmstate_vm *vm = &st->vm;
    vm->pic_master.chip_id = KVM_IRQCHIP_PIC_MASTER;
    vm->pic_slave.chip_id = KVM_IRQCHIP_PIC_SLAVE;
    vm->ioapic.chip_id = KVM_IRQCHIP_IOAPIC;
    if (xioctl(kvm->vm_fd, KVM_GET_IRQCHIP, &vm->pic_master, "KVM_GET_IRQCHIP(PIC master)") < 0 ||
        xioctl(kvm->vm_fd, KVM_GET_IRQCHIP, &vm->pic_slave, "KVM_GET_IRQCHIP(PIC slave)") < 0 ||
        xioctl(kvm->vm_fd, KVM_GET_IRQCHIP, &vm->ioapic, "KVM_GET_IRQCHIP(IOAPIC)") < 0 ||
        xioctl(kvm->vm_fd, KVM_GET_PIT2, &vm->pit, "KVM_GET_PIT2") < 0 ||
        xioctl(kvm->vm_fd, KVM_GET_CLOCK, &vm->clock, "KVM_GET_CLOCK") < 0)
        return -1;

    /*
     * KVM_GET_XSAVE 只能装下 4 KiB；客户机启用了更大的状态（AMX）时要用
     * KVM_GET_XSAVE2。SVMM 交给客户机的 CPUID 里没有 AMX（没有申请 guest 权限），
     * 这里只做检查，万一超了就拒绝保存，而不是存下半份 FPU 状态。
     */
    int xsave_size = ioctl(kvm->vm_fd, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE2);
    if (xsave_size > (int)sizeof(struct kvm_xsave)) {
        fprintf(stderr, "ERROR: vmstate: guest XSAVE state is %d bytes, more than %zu "
                "supported\n", xsave_size, sizeof(struct kvm_xsave));
        return -1;
    }

    struct vmstate_cpu *cpu = &st->cpu;
    int fd = vcpu->fd;
    if (get_cpuid(fd, cpu) < 0 || get_tsc_khz(fd, &cpu->tsc_khz) < 0 ||
        xioctl(fd, KVM_GET_MP_STATE, &cpu->mp_state, "KVM_GET_MP_STATE") < 0 ||
        xioctl(fd, KVM_GET_REGS, &cpu->regs, "KVM_GET_REGS") < 0 ||
        xioctl(fd, KVM_GET_SREGS, &cpu->sregs, "KVM_GET_SREGS") < 0 ||
        xioctl(fd, KVM_GET_XSAVE, &cpu->xsave, "KVM_GET_XSAVE") < 0 ||
        xioctl(fd, KVM_GET_XCRS, &cpu->xcrs, "KVM_GET_XCRS") < 0 ||
        xioctl(fd, KVM_GET_DEBUGREGS, &cpu->debugregs, "KVM_GET_DEBUGREGS") < 0 ||
        xioctl(fd, KVM_GET_LAPIC, &cpu->lapic, "KVM_GET_LAPIC") < 0 ||
        get_msrs(kvm, fd, cpu) < 0 ||
        xioctl(fd, KVM_GET_VCPU_EVENTS, &cpu->events, "KVM_GET_VCPU_EVENTS") < 0)
        return -1;
    return 0;
}

/*
 * 刚 vcpu_init() 的 vCPU 已经按和 boot 时相同的方法装好了 CPUID。保存时的客户机内核
 * 早就按那时的 CPUID 选好了代码路径（用不用 AVX-512、XSAVES、TSC deadline……），
 * 只要有一项不同就可能在恢复后执行非法指令，所以要求完全一致。TSC 频率同理：
 * 客户机按保存时的频率把 TSC 换算成时间。
 */
/*
 * TSC 频率在写任何状态之前比较：客户机的 kvmclock/TSC 换算系数是按它算好的。
 */
static int check_tsc(const struct vmstate_cpu *saved, int vcpu_fd)
{
    uint32_t khz;
    if (get_tsc_khz(vcpu_fd, &khz) < 0)
        return -1;
    if (khz != saved->tsc_khz) {
        fprintf(stderr, "ERROR: vmstate: TSC frequency %u kHz differs from the snapshot's "
                "%u kHz\n", khz, saved->tsc_khz);
        return -1;
    }
    return 0;
}

/*
 * CPUID 在所有寄存器写回之后比较。KVM_GET_CPUID2 返回的不只是 VMM 设置的表：
 * 有几项由 KVM 按客户机当前状态实时改写——leaf 1 ECX.OSXSAVE（CR4.OSXSAVE）、
 * leaf 7 ECX.OSPKE（CR4.PKE）、leaf 1 EDX.APIC（APIC_BASE 的使能位）、leaf 0xD
 * 的 XSAVE 区大小（XCR0 / IA32_XSS）。刚 vcpu_init() 的 vCPU 上这些还是复位值，
 * 必须等 SREGS / XCRS / MSR 写回、KVM 重新算过之后，同一台宿主机上的两张表才逐字节
 * 相同；剩下的差异才真的意味着“换了 CPU 或 KVM”。
 */
static int check_cpuid(const struct vmstate_cpu *saved, int vcpu_fd)
{
    struct vmstate_cpu *now = calloc(1, sizeof(*now));
    int ret = -1;
    if (!now) {
        fprintf(stderr, "ERROR: vmstate: out of memory\n");
        return -1;
    }
    if (get_cpuid(vcpu_fd, now) < 0)
        goto out;
    if (now->ncpuid != saved->ncpuid ||
        memcmp(now->cpuid, saved->cpuid, now->ncpuid * sizeof(now->cpuid[0]))) {
        fprintf(stderr, "ERROR: vmstate: CPUID of this host/KVM differs from the one the "
                "snapshot was taken with; restore on the same host\n");
        goto out;
    }
    ret = 0;
out:
    free(now);
    return ret;
}

static int set_msrs(int vcpu_fd, const struct vmstate_cpu *cpu)
{
    struct kvm_msrs *msrs = calloc(1, sizeof(*msrs) + cpu->nmsrs * sizeof(msrs->entries[0]));
    if (!msrs) {
        fprintf(stderr, "ERROR: vmstate: out of memory\n");
        return -1;
    }
    msrs->nmsrs = cpu->nmsrs;
    memcpy(msrs->entries, cpu->msrs, cpu->nmsrs * sizeof(msrs->entries[0]));
    /* 返回成功写入的个数：少一个就意味着那个 MSR 的值没恢复，不能装作没事。 */
    int set = ioctl(vcpu_fd, KVM_SET_MSRS, msrs);
    int ret = 0;
    if (set < 0) {
        fprintf(stderr, "ERROR: vmstate: KVM_SET_MSRS: %s\n", strerror(errno));
        ret = -1;
    } else if ((uint32_t)set != cpu->nmsrs) {
        fprintf(stderr, "ERROR: vmstate: KVM_SET_MSRS rejected MSR 0x%x (%d of %u set)\n",
                cpu->msrs[set].index, set, cpu->nmsrs);
        ret = -1;
    }
    free(msrs);
    return ret;
}

int vmstate_apply(const struct vmstate *st, const struct kvm_context *kvm,
                  const struct vcpu *vcpu)
{
    const struct vmstate_cpu *cpu = &st->cpu;
    int fd = vcpu->fd;
    if (check_tsc(cpu, fd) < 0)
        return -1;

    struct vmstate_vm vm = st->vm;
    /* flags=0：只设置 kvmclock 的值，客户机时间从暂停那一刻接着走。 */
    vm.clock.flags = 0;
    if (xioctl(kvm->vm_fd, KVM_SET_IRQCHIP, &vm.pic_master, "KVM_SET_IRQCHIP(PIC master)") < 0 ||
        xioctl(kvm->vm_fd, KVM_SET_IRQCHIP, &vm.pic_slave, "KVM_SET_IRQCHIP(PIC slave)") < 0 ||
        xioctl(kvm->vm_fd, KVM_SET_IRQCHIP, &vm.ioapic, "KVM_SET_IRQCHIP(IOAPIC)") < 0 ||
        xioctl(kvm->vm_fd, KVM_SET_PIT2, &vm.pit, "KVM_SET_PIT2") < 0 ||
        xioctl(kvm->vm_fd, KVM_SET_CLOCK, &vm.clock, "KVM_SET_CLOCK") < 0)
        return -1;

    struct vmstate_cpu *c = calloc(1, sizeof(*c));
    if (!c) {
        fprintf(stderr, "ERROR: vmstate: out of memory\n");
        return -1;
    }
    *c = *cpu;
    c->events.flags |= KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SIPI_VECTOR;
    int ret = -1;
    if (xioctl(fd, KVM_SET_MP_STATE, &c->mp_state, "KVM_SET_MP_STATE") < 0 ||
        xioctl(fd, KVM_SET_REGS, &c->regs, "KVM_SET_REGS") < 0 ||
        xioctl(fd, KVM_SET_SREGS, &c->sregs, "KVM_SET_SREGS") < 0 ||
        xioctl(fd, KVM_SET_XSAVE, &c->xsave, "KVM_SET_XSAVE") < 0 ||
        xioctl(fd, KVM_SET_XCRS, &c->xcrs, "KVM_SET_XCRS") < 0 ||
        xioctl(fd, KVM_SET_DEBUGREGS, &c->debugregs, "KVM_SET_DEBUGREGS") < 0 ||
        xioctl(fd, KVM_SET_LAPIC, &c->lapic, "KVM_SET_LAPIC") < 0 ||
        set_msrs(fd, c) < 0 ||
        xioctl(fd, KVM_SET_VCPU_EVENTS, &c->events, "KVM_SET_VCPU_EVENTS") < 0 ||
        check_cpuid(cpu, fd) < 0)
        goto out;
    ret = 0;
out:
    free(c);
    return ret;
}

/* ---------- 编码 ---------- */

struct writer {
    uint8_t *buf;
    size_t len, cap;
    uint32_t count;
};

static int put(struct writer *w, const void *data, size_t len)
{
    if (w->len + len > w->cap) {
        size_t cap = w->cap ? w->cap : 64 * 1024;
        while (cap < w->len + len)
            cap *= 2;
        uint8_t *grown = realloc(w->buf, cap);
        if (!grown)
            return -1;
        w->buf = grown;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
    return 0;
}

static int put_u32(struct writer *w, uint32_t v)
{
    return put(w, &v, sizeof(v));
}

/* 写一个 section。has_count 时 payload 前面是 u32 元素个数 + 4 字节填充（数组 section）。 */
static int section(struct writer *w, uint32_t tag, const void *data, size_t len,
                   bool has_count, uint32_t count)
{
    size_t total = len + (has_count ? 8 : 0);
    if (put_u32(w, tag) < 0 || put_u32(w, (uint32_t)total) < 0 ||
        (has_count && (put_u32(w, count) < 0 || put_u32(w, 0) < 0)) || put(w, data, len) < 0)
        return -1;
    w->count++;
    return 0;
}

int vmstate_encode(const struct vmstate *st, uint8_t **out, size_t *len)
{
    struct writer w = { 0 };
    const struct vmstate_cpu *c = &st->cpu;
    const struct vmstate_vm *vm = &st->vm;
    int bad = put(&w, VMSTATE_MAGIC, 8) < 0 || put_u32(&w, VMSTATE_VERSION) < 0 ||
              put_u32(&w, 0) < 0 ||
              section(&w, VMSTATE_TAG_PIC_MASTER, &vm->pic_master, sizeof(vm->pic_master), 0, 0) ||
              section(&w, VMSTATE_TAG_PIC_SLAVE, &vm->pic_slave, sizeof(vm->pic_slave), 0, 0) ||
              section(&w, VMSTATE_TAG_IOAPIC, &vm->ioapic, sizeof(vm->ioapic), 0, 0) ||
              section(&w, VMSTATE_TAG_PIT, &vm->pit, sizeof(vm->pit), 0, 0) ||
              section(&w, VMSTATE_TAG_CLOCK, &vm->clock, sizeof(vm->clock), 0, 0) ||
              section(&w, VMSTATE_TAG_CPUID, c->cpuid, c->ncpuid * sizeof(c->cpuid[0]), 1,
                      c->ncpuid) ||
              section(&w, VMSTATE_TAG_TSC_KHZ, &c->tsc_khz, sizeof(c->tsc_khz), 0, 0) ||
              section(&w, VMSTATE_TAG_MP_STATE, &c->mp_state, sizeof(c->mp_state), 0, 0) ||
              section(&w, VMSTATE_TAG_REGS, &c->regs, sizeof(c->regs), 0, 0) ||
              section(&w, VMSTATE_TAG_SREGS, &c->sregs, sizeof(c->sregs), 0, 0) ||
              section(&w, VMSTATE_TAG_XSAVE, &c->xsave, sizeof(c->xsave), 0, 0) ||
              section(&w, VMSTATE_TAG_XCRS, &c->xcrs, sizeof(c->xcrs), 0, 0) ||
              section(&w, VMSTATE_TAG_DEBUGREGS, &c->debugregs, sizeof(c->debugregs), 0, 0) ||
              section(&w, VMSTATE_TAG_LAPIC, &c->lapic, sizeof(c->lapic), 0, 0) ||
              section(&w, VMSTATE_TAG_MSRS, c->msrs, c->nmsrs * sizeof(c->msrs[0]), 1,
                      c->nmsrs) ||
              section(&w, VMSTATE_TAG_EVENTS, &c->events, sizeof(c->events), 0, 0) ||
              section(&w, VMSTATE_TAG_SERIAL, &st->serial, sizeof(st->serial), 0, 0) ||
              ((st->devices & VMSTATE_DEV_BLK) &&
               section(&w, VMSTATE_TAG_VIRTIO_BLK, &st->blk, sizeof(st->blk), 0, 0)) ||
              ((st->devices & VMSTATE_DEV_NET) &&
               section(&w, VMSTATE_TAG_VIRTIO_NET, &st->net, sizeof(st->net), 0, 0));
    if (bad) {
        free(w.buf);
        fprintf(stderr, "ERROR: vmstate: out of memory\n");
        return -1;
    }
    memcpy(w.buf + 12, &w.count, sizeof(w.count));
    *out = w.buf;
    *len = w.len;
    return 0;
}

/* ---------- 解码 ---------- */

struct section_spec {
    uint32_t tag;
    const char *name;
    size_t offset; /* 在 struct vmstate 里的位置 */
    size_t size;   /* 定长 section 的长度；数组 section 是单个元素的长度 */
    size_t count_offset; /* 数组 section 的计数字段位置；定长时为 0 */
    uint32_t max;
    bool optional;
};

#define FIXED(tag, field) { tag, #field, offsetof(struct vmstate, field), \
                            sizeof(((struct vmstate *)0)->field), 0, 0, false }
#define ARRAY(tag, field, count, maxn) { tag, #field, offsetof(struct vmstate, field), \
                                         sizeof(((struct vmstate *)0)->field[0]), \
                                         offsetof(struct vmstate, count), maxn, false }

static const struct section_spec specs[] = {
    FIXED(VMSTATE_TAG_PIC_MASTER, vm.pic_master),
    FIXED(VMSTATE_TAG_PIC_SLAVE, vm.pic_slave),
    FIXED(VMSTATE_TAG_IOAPIC, vm.ioapic),
    FIXED(VMSTATE_TAG_PIT, vm.pit),
    FIXED(VMSTATE_TAG_CLOCK, vm.clock),
    ARRAY(VMSTATE_TAG_CPUID, cpu.cpuid, cpu.ncpuid, VMSTATE_MAX_CPUID),
    FIXED(VMSTATE_TAG_TSC_KHZ, cpu.tsc_khz),
    FIXED(VMSTATE_TAG_MP_STATE, cpu.mp_state),
    FIXED(VMSTATE_TAG_REGS, cpu.regs),
    FIXED(VMSTATE_TAG_SREGS, cpu.sregs),
    FIXED(VMSTATE_TAG_XSAVE, cpu.xsave),
    FIXED(VMSTATE_TAG_XCRS, cpu.xcrs),
    FIXED(VMSTATE_TAG_DEBUGREGS, cpu.debugregs),
    FIXED(VMSTATE_TAG_LAPIC, cpu.lapic),
    ARRAY(VMSTATE_TAG_MSRS, cpu.msrs, cpu.nmsrs, VMSTATE_MAX_MSRS),
    FIXED(VMSTATE_TAG_EVENTS, cpu.events),
    FIXED(VMSTATE_TAG_SERIAL, serial),
    { VMSTATE_TAG_VIRTIO_BLK, "virtio-blk", offsetof(struct vmstate, blk),
      sizeof(struct virtio_mmio_state), 0, 0, true },
    { VMSTATE_TAG_VIRTIO_NET, "virtio-net", offsetof(struct vmstate, net),
      sizeof(struct virtio_mmio_state), 0, 0, true },
};

#define NSPECS (sizeof(specs) / sizeof(specs[0]))

int vmstate_decode(struct vmstate *st, const uint8_t *buf, size_t len)
{
    memset(st, 0, sizeof(*st));
    uint32_t version, count;
    if (len < 16 || memcmp(buf, VMSTATE_MAGIC, 8)) {
        fprintf(stderr, "ERROR: vmstate: bad magic\n");
        return -1;
    }
    memcpy(&version, buf + 8, 4);
    memcpy(&count, buf + 12, 4);
    if (version != VMSTATE_VERSION) {
        fprintf(stderr, "ERROR: vmstate: unsupported version %u (expected %u)\n", version,
                VMSTATE_VERSION);
        return -1;
    }
    bool seen[NSPECS] = { false };
    size_t off = 16;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t tag, slen;
        if (len - off < 8) {
            fprintf(stderr, "ERROR: vmstate: truncated section header\n");
            return -1;
        }
        memcpy(&tag, buf + off, 4);
        memcpy(&slen, buf + off + 4, 4);
        off += 8;
        if (slen > len - off) {
            fprintf(stderr, "ERROR: vmstate: section %u is truncated\n", tag);
            return -1;
        }
        size_t k = 0;
        while (k < NSPECS && specs[k].tag != tag)
            k++;
        if (k == NSPECS) {
            fprintf(stderr, "ERROR: vmstate: unknown section tag %u\n", tag);
            return -1;
        }
        const struct section_spec *s = &specs[k];
        if (seen[k]) {
            fprintf(stderr, "ERROR: vmstate: duplicate section %s\n", s->name);
            return -1;
        }
        seen[k] = true;
        uint8_t *dst = (uint8_t *)st + s->offset;
        if (s->count_offset) {
            uint32_t n;
            if (slen < 8 || (memcpy(&n, buf + off, 4), n > s->max) ||
                slen != 8 + (size_t)n * s->size) {
                fprintf(stderr, "ERROR: vmstate: section %s has a bad length\n", s->name);
                return -1;
            }
            memcpy((uint8_t *)st + s->count_offset, &n, 4);
            memcpy(dst, buf + off + 8, (size_t)n * s->size);
        } else {
            if (slen != s->size) {
                fprintf(stderr, "ERROR: vmstate: section %s is %u bytes, expected %zu\n",
                        s->name, slen, s->size);
                return -1;
            }
            memcpy(dst, buf + off, slen);
        }
        off += slen;
    }
    if (off != len) {
        fprintf(stderr, "ERROR: vmstate: %zu trailing bytes\n", len - off);
        return -1;
    }
    for (size_t k = 0; k < NSPECS; k++) {
        if (!seen[k] && !specs[k].optional) {
            fprintf(stderr, "ERROR: vmstate: missing section %s\n", specs[k].name);
            return -1;
        }
    }
    if (st->vm.pic_master.chip_id != KVM_IRQCHIP_PIC_MASTER ||
        st->vm.pic_slave.chip_id != KVM_IRQCHIP_PIC_SLAVE ||
        st->vm.ioapic.chip_id != KVM_IRQCHIP_IOAPIC) {
        fprintf(stderr, "ERROR: vmstate: irqchip sections have wrong chip ids\n");
        return -1;
    }
    st->devices = (seen[NSPECS - 2] ? VMSTATE_DEV_BLK : 0) |
                  (seen[NSPECS - 1] ? VMSTATE_DEV_NET : 0);
    return 0;
}

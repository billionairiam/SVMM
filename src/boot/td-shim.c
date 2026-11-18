#include "tdx.h"
#include "boot/acpi.h"
#include "boot/linux.h"
#include "memory.h"

#include <asm/bootparam.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SECTIONS 32u
#define MAX_FIRMWARE (64u * MIB)
#define TOP_4G (UINT64_C(1) << 32)
#define FIRMWARE_MIN_GPA (LAPIC_ADDR + TDX_PAGE_SIZE)
#define ALIGN_PAGE(n) (((n) + TDX_PAGE_SIZE - 1) & ~(uint64_t)(TDX_PAGE_SIZE - 1))
/* TD-Shim boot_kernel runtime layout: ACPI, page table, mailbox and event log. */
#define SHIM_RUNTIME_RESERVE (0x100000u + 0x20000u + 0x2000u + 0x100000u)

enum section_type { BFV, CFV, TD_HOB, TEMP_MEM, PERM_MEM, PAYLOAD, PAYLOAD_PARAM };

struct section {
    uint32_t raw_offset, raw_size;
    uint64_t address, size;
    uint32_t type, flags;
};

struct td_shim {
    size_t file_size, count;
    uint8_t *file;
    struct section sections[MAX_SECTIONS];
    struct section hob, payload, parameter;
    uint64_t high_base;
};

static int within(uint64_t start, uint64_t size, uint64_t limit)
{
    return start <= limit && size <= limit - start;
}

static int overlap(uint64_t start, uint64_t size, uint64_t other, uint64_t length)
{
    return size && length && start < other + length && other < start + size;
}

static uint32_t u32_at(const uint8_t *source)
{
    uint32_t value;
    memcpy(&value, source, sizeof(value));
    return value;
}

static int parse_metadata(struct td_shim *shim)
{
    static const uint8_t guid[] = {
        0xf3, 0xf9, 0xea, 0xe9, 0x8e, 0x16, 0xd5, 0x44,
        0xa8, 0xeb, 0x7f, 0x4d, 0x87, 0x38, 0xf6, 0xae,
    };
    uint32_t offset = u32_at(shim->file + shim->file_size - 32);
    if (offset < sizeof(guid) || !within(offset, 16, shim->file_size))
        return -1;
    const uint8_t *metadata = shim->file + offset;
    if (memcmp(metadata - sizeof(guid), guid, sizeof(guid)) ||
        memcmp(metadata, "TDVF", 4) || u32_at(metadata + 8) != 1)
        return -1;
    uint32_t length = u32_at(metadata + 4), count = u32_at(metadata + 12);
    if (!count || count > MAX_SECTIONS || length != 16 + count * sizeof(struct section) ||
        !within(offset, length, shim->file_size - 32))
        return -1;

    int reset_vector = 0;
    shim->high_base = TOP_4G;
    shim->count = count;
    memcpy(shim->sections, metadata + 16, count * sizeof(struct section));
    for (size_t i = 0; i < count; ++i) {
        const struct section *s = &shim->sections[i];
        if (!s->size || (s->address | s->size) % TDX_PAGE_SIZE ||
            s->raw_size > s->size || !within(s->raw_offset, s->raw_size, shim->file_size) ||
            !within(s->address, s->size, TOP_4G) || s->type > PAYLOAD_PARAM || s->flags & ~1u ||
            (!s->raw_size && s->raw_offset))
            return -1;
        for (size_t j = 0; j < i; ++j) {
            const struct section *other = &shim->sections[j];
            if (overlap(s->address, s->size, other->address, other->size))
                return -1;
        }
        if (s->address >= FIRMWARE_MIN_GPA) {
            if (s->type == BFV) {
                if (reset_vector || s->raw_size < TDX_PAGE_SIZE || s->flags != 1 ||
                    s->address + s->raw_size != TOP_4G ||
                    (uint64_t)s->raw_offset + s->raw_size != shim->file_size)
                    return -1;
                reset_vector = 1;
            } else if (s->type == CFV) {
                if (!s->raw_size || s->flags)
                    return -1;
            } else if (s->type != TEMP_MEM || s->raw_size || s->flags) {
                return -1;
            }
            if (s->address < shim->high_base)
                shim->high_base = s->address;
            continue;
        }

        /* This backend supports the boot_kernel layout, with no built-in
         * payload or firmware-managed permanent memory. */
        if (s->raw_size || s->address < ACPI_TABLES_END ||
            !within(s->address, s->size, (uint64_t)GUEST_MEMORY_MAX_MB * MIB))
            return -1;
        if (s->type == TD_HOB && !s->flags && !shim->hob.size)
            shim->hob = *s;
        else if (s->type == PAYLOAD && !shim->payload.size)
            shim->payload = *s;
        else if (s->type == PAYLOAD_PARAM && !s->flags && !shim->parameter.size)
            shim->parameter = *s;
        else
            return -1;
    }
    if (!reset_vector || TOP_4G - shim->high_base > MAX_FIRMWARE ||
        !shim->hob.size || !shim->payload.size || !shim->parameter.size ||
        shim->parameter.address < shim->hob.address ||
        shim->payload.address < shim->parameter.address)
        return -1;
    return 0;
}

struct td_shim *td_shim_open(const char *path)
{
    struct td_shim *shim = calloc(1, sizeof(*shim));
    if (!shim) {
        perror("allocate TD-Shim metadata");
        return NULL;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "ERROR: cannot open TD-Shim %s: %m\n", path);
        goto fail;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < TDX_PAGE_SIZE ||
        st.st_size > MAX_FIRMWARE) {
        fprintf(stderr, "ERROR: invalid TD-Shim image size or type: %s\n", path);
        close(fd);
        goto fail;
    }
    shim->file_size = (size_t)st.st_size;
    shim->file = malloc(shim->file_size);
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        goto fail;
    }
    size_t got = shim->file ? fread(shim->file, 1, shim->file_size, file) : 0;
    fclose(file);
    if (got != shim->file_size || parse_metadata(shim) < 0) {
        fprintf(stderr, "ERROR: invalid or unsupported TD-Shim metadata: %s\n", path);
        goto fail;
    }
    return shim;
fail:
    td_shim_close(shim);
    return NULL;
}

void td_shim_close(struct td_shim *shim)
{
    if (shim) {
        free(shim->file);
        free(shim);
    }
}

struct hob_header {
    uint16_t type, length;
    uint32_t reserved;
};

struct hob_phit {
    struct hob_header header;
    uint32_t version, boot_mode;
    uint64_t memory_top, memory_bottom, free_top, free_bottom, end;
};

struct hob_memory {
    struct hob_header header;
    uint8_t owner[16];
    uint32_t type, attributes;
    uint64_t start, size;
};

struct hob_guid {
    struct hob_header header;
    uint8_t guid[16];
};

struct hob_writer {
    uint8_t *data;
    size_t size, used;
};

static int append(struct hob_writer *w, const void *data, size_t len)
{
    if (len > w->size - w->used)
        return -1;
    memcpy(w->data + w->used, data, len);
    w->used += len;
    return 0;
}

static int append_guid(struct hob_writer *w, const uint8_t guid[16],
                       const void *data, size_t size)
{
    struct hob_guid header = { .header = { .type = 4, .length = sizeof(header) + size } };
    memcpy(header.guid, guid, 16);
    const uint8_t padding[8] = { 0 };
    return append(w, &header, sizeof(header)) || append(w, data, size) ||
           append(w, padding, (8 - size % 8) % 8) ? -1 : 0;
}

static int build_hob(const struct section *section, struct guest_memory *ram, uint64_t entry)
{
    static const uint8_t payload_guid[16] = {
        0x12, 0xa4, 0x6f, 0xb9, 0x1f, 0x46, 0xe3, 0x4b,
        0x8c, 0x0d, 0xad, 0x80, 0x5a, 0x49, 0x7a, 0xc0,
    };
    static const uint8_t acpi_guid[16] = {
        0x70, 0x58, 0x0c, 0x6a, 0xed, 0xd4, 0xf4, 0x44,
        0xa1, 0x35, 0xdd, 0x23, 0x8b, 0x6f, 0x0c, 0x8d,
    };
    struct hob_phit phit = {
        .header = { .type = 1, .length = sizeof(phit) },
        .version = 9,
    };
    struct hob_memory memory = {
        .header = { .type = 3, .length = sizeof(memory) },
        .attributes = 7, .size = ram->size,
    };
    struct {
        uint32_t format, reserved;
        uint64_t entry;
    } payload = { .format = 2, .entry = entry }; /* RawVmLinux */
    struct hob_header end = { .type = 0xffff, .length = sizeof(end) };
    struct hob_writer w = { .data = ram->data + section->address, .size = section->size };
    if (append(&w, &phit, sizeof(phit)) || append(&w, &memory, sizeof(memory)) ||
        append_guid(&w, payload_guid, &payload, sizeof(payload)))
        return -1;
    const unsigned tables[] = { ACPI_FADT_ADDR, ACPI_DSDT_ADDR, ACPI_MADT_ADDR };
    for (size_t i = 0; i < sizeof(tables) / sizeof(*tables); ++i) {
        uint8_t *table = ram->data + tables[i];
        uint32_t size = u32_at(table + 4);
        if (tables[i] == ACPI_FADT_ADDR) {
            /* TD-Shim relocates the 32-bit pointer and recomputes checksum. */
            memset(table + 140, 0, 8);
        }
        if (append_guid(&w, acpi_guid, table, size) < 0)
            return -1;
    }
    if (append(&w, &end, sizeof(end)))
        return -1;
    phit.end = section->address + w.used;
    memcpy(w.data, &phit, sizeof(phit));
    return 0;
}

/* TD-Shim passes its own zero page in RSI, with its ACPI pointer and e820.
 * Copy only the setup_header from our template, then enter the bzImage's
 * documented 64-bit entry. Keep RSI intact for Linux. */
static void stage_linux_stub(uint8_t *dest, const struct setup_header *hdr,
                             uint32_t kernel_entry)
{
    uint8_t stub[] = {
        0xfc,                                  /* cld */
        0x56,                                  /* push rsi */
        0x48, 0x8d, 0xbe, 0xf1, 0x01, 0, 0,  /* lea rdi, [rsi+0x1f1] */
        0x48, 0x8d, 0x35, 0, 0, 0, 0,        /* lea rsi, [rip+header] */
        0xb9, 0, 0, 0, 0,                    /* mov ecx, sizeof(header) */
        0xf3, 0xa4,                            /* rep movsb */
        0x5e,                                  /* pop rsi */
        0xb8, 0, 0, 0, 0,                    /* mov eax, startup_64 */
        0xff, 0xe0,                            /* jmp rax */
    };
    uint32_t relative = sizeof(stub) - 16, size = sizeof(*hdr);
    memcpy(stub + 12, &relative, 4);
    memcpy(stub + 17, &size, 4);
    memcpy(stub + 25, &kernel_entry, 4);
    memcpy(dest, stub, sizeof(stub));
    memcpy(dest + sizeof(stub), hdr, sizeof(*hdr));
}

int td_shim_stage(const struct td_shim *shim, struct tdx_vm *td,
                  const struct linux_image *image, const struct linux_initrd *initrd,
                  const char *cmdline, uint64_t *hob, struct tdx_range measure[3])
{
    struct guest_memory *ram = td->ram;
    struct setup_header header = { 0 };
    enum { header_offset = 0x1f1 };
    if (image->setup_size < header_offset + sizeof(header))
        goto invalid;
    memcpy(&header, image->data + header_offset, sizeof(header));
    uint64_t kernel = image->runtime_start;
    uint64_t initrd_size = ALIGN_PAGE(initrd->size);
    uint64_t limit = ram->size - SHIM_RUNTIME_RESERVE;
    if (!within(kernel, image->init_size, limit) ||
        (initrd->size && !within(initrd->addr, initrd_size, limit))) {
        fprintf(stderr, "ERROR: kernel or initrd overlaps the TD-Shim runtime reservation\n");
        return -1;
    }
    if (!(header.xloadflags & XLF_KERNEL_64) || kernel < shim->payload.address + TDX_PAGE_SIZE ||
        !within(kernel, image->kernel_size, shim->payload.address + shim->payload.size) ||
        image->kernel_size <= 0x200 ||
        !within(shim->payload.address, shim->payload.size, limit) ||
        (initrd->size && (initrd->addr < kernel + image->init_size ||
          initrd->addr < ALIGN_PAGE(kernel + image->kernel_size) ||
          initrd->addr % TDX_PAGE_SIZE)) ||
        strlen(cmdline) >= shim->parameter.size ||
        (header.cmdline_size && strlen(cmdline) > header.cmdline_size))
        goto invalid;
    td->firmware_base = shim->high_base;
    td->firmware_size = TOP_4G - shim->high_base;
    void *high = mmap(NULL, td->firmware_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (high == MAP_FAILED) {
        perror("allocate TD-Shim staging memory");
        return -1;
    }
    td->firmware = high;
    for (size_t i = 0; i < shim->count; ++i) {
        const struct section *s = &shim->sections[i];
        if (!s->raw_size)
            continue;
        memcpy(td->firmware + (s->address - td->firmware_base),
               shim->file + s->raw_offset, s->raw_size);
    }
    header.type_of_loader = 0xff;
    header.code32_start = (uint32_t)kernel;
    header.cmd_line_ptr = (uint32_t)shim->parameter.address;
    header.ramdisk_image = initrd->size ? (uint32_t)initrd->addr : 0;
    header.ramdisk_size = (uint32_t)initrd->size;
    header.setup_data = 0;
    memcpy(ram->data + header.cmd_line_ptr, cmdline, strlen(cmdline) + 1);
    memcpy(ram->data + kernel, image->data + image->setup_size, image->kernel_size);
    stage_linux_stub(ram->data + shim->payload.address, &header, (uint32_t)kernel + 0x200);
    if (build_hob(&shim->hob, ram, shim->payload.address) < 0)
        goto invalid;

    *hob = shim->hob.address;
    measure[0] = (struct tdx_range){ *hob, ALIGN_PAGE(kernel + image->kernel_size) - *hob };
    measure[1] = (struct tdx_range){ initrd->addr, initrd_size };
    return 0;
invalid:
    fprintf(stderr, "ERROR: incompatible TD-Shim, bzImage or initrd layout\n");
    return -1;
}

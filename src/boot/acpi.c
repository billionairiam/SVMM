#include "boot/acpi.h"

#include "memory.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/*
 * 生成一组最小 ACPI 表，放在 0xE0000 起的 BIOS 区域：
 *
 *   0xE0000  RSDP  入口指针，内核扫描 0xE0000–0xFFFFF 或按 acpi_rsdp= 找到它
 *   0xE1000  RSDT  指向 FADT 和 MADT
 *   0xE2000  FADT  硬件精简（HW_REDUCED_ACPI）模式，指向 DSDT，声明复位和
 *                  睡眠控制寄存器
 *   0xE3000  DSDT  AML：\_S5 关机类型，\_SB.COM1（PNP0501，0x3f8–0x3ff，IRQ 4）
 *   0xE4000  MADT  CPU 0 的 Local APIC 和 KVM 模拟的 I/O APIC
 *
 * 硬件精简模式不需要 PM1 事件/控制块、PM 定时器和 SCI，因此 VMM 只需
 * 拦截三个 8 位端口：睡眠控制（关机）、睡眠状态、复位。Linux 在这种模式下
 * 不使用 8259 和 PIT，中断经 I/O APIC 投递，定时器使用 LAPIC TSC-deadline。
 *
 * 每张表的 checksum 字节使整张表所有字节之和为 0（mod 256）。
 */

#define ACPI_HEADER_SIZE 36u
#define RSDP_SIZE 36u
#define RSDT_SIZE (ACPI_HEADER_SIZE + 2u * 4u)
#define FADT_SIZE 276u
#define MADT_SIZE (ACPI_HEADER_SIZE + 8u + 8u + 12u)

#define FADT_WBINVD (1u << 0)
#define FADT_PWR_BUTTON (1u << 4)
#define FADT_SLP_BUTTON (1u << 5)
#define FADT_RESET_REG_SUP (1u << 10)
#define FADT_HW_REDUCED_ACPI (1u << 20)

#define IAPC_LEGACY_DEVICES (1u << 0)
#define IAPC_VGA_NOT_PRESENT (1u << 2)
#define IAPC_CMOS_RTC_NOT_PRESENT (1u << 5)

#define GAS_SYSTEM_IO 1u
#define GAS_ACCESS_BYTE 1u

/*
 * DSDT 的 AML 定义块主体（不含 36 字节表头），由下面的 ASL 经
 * `iasl -tc` 编译后截取：
 *
 *   DefinitionBlock ("dsdt.aml", "DSDT", 2, "SVMM  ", "SANDBOX ", 1)
 *   {
 *       Name (_S5, Package (0x04) { 0x05, 0x05, Zero, Zero })
 *       Scope (\_SB)
 *       {
 *           Device (COM1)
 *           {
 *               Name (_HID, EisaId ("PNP0501"))
 *               Name (_UID, Zero)
 *               Name (_CRS, ResourceTemplate ()
 *               {
 *                   IO (Decode16, 0x03F8, 0x03F8, 0x01, 0x08)
 *                   IRQNoFlags () {4}
 *               })
 *           }
 *       }
 *   }
 *
 * _S5 的第一个元素就是写入睡眠控制寄存器的 SLP_TYP，必须与
 * ACPI_SLEEP_TYPE_S5 一致。
 */
static const uint8_t dsdt_aml[] = {
    /* Name (_S5, Package (4) { 5, 5, 0, 0 }) */
    0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x08, 0x04,
    0x0a, ACPI_SLEEP_TYPE_S5, 0x0a, ACPI_SLEEP_TYPE_S5, 0x00, 0x00,
    /* Scope (\_SB) */
    0x10, 0x32, 0x5f, 0x53, 0x42, 0x5f,
    /* Device (COM1) */
    0x5b, 0x82, 0x2b, 0x43, 0x4f, 0x4d, 0x31,
    /* Name (_HID, EisaId ("PNP0501")) */
    0x08, 0x5f, 0x48, 0x49, 0x44, 0x0c, 0x41, 0xd0, 0x05, 0x01,
    /* Name (_UID, Zero) */
    0x08, 0x5f, 0x55, 0x49, 0x44, 0x00,
    /* Name (_CRS, Buffer (16) { ... }) */
    0x08, 0x5f, 0x43, 0x52, 0x53, 0x11, 0x10, 0x0a, 0x0d,
    /* IO (Decode16, 0x3f8, 0x3f8, 1, 8) */
    0x47, 0x01, 0xf8, 0x03, 0xf8, 0x03, 0x01, 0x08,
    /* IRQNoFlags () {4}：IRQ 位图 bit 4 */
    0x22, 0x10, 0x00,
    /* EndTag */
    0x79, 0x00,
};

#define DSDT_SIZE (ACPI_HEADER_SIZE + sizeof(dsdt_aml))

static void put8(uint8_t *table, size_t offset, uint8_t value)
{
    table[offset] = value;
}

static void put16(uint8_t *table, size_t offset, uint16_t value)
{
    table[offset] = (uint8_t)value;
    table[offset + 1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *table, size_t offset, uint32_t value)
{
    put16(table, offset, (uint16_t)value);
    put16(table, offset + 2, (uint16_t)(value >> 16));
}

static void put64(uint8_t *table, size_t offset, uint64_t value)
{
    put32(table, offset, (uint32_t)value);
    put32(table, offset + 4, (uint32_t)(value >> 32));
}

static uint8_t checksum(const uint8_t *data, size_t size)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < size; ++i)
        sum = (uint8_t)(sum + data[i]);
    return (uint8_t)(0u - sum);
}

/* 所有 SDT 共用的 36 字节表头；checksum 在表内容填完后再计算。 */
static void put_header(uint8_t *table, const char signature[4], uint32_t length,
                       uint8_t revision)
{
    memcpy(table, signature, 4);
    put32(table, 4, length);
    put8(table, 8, revision);
    memcpy(table + 10, "SVMM  ", 6);
    memcpy(table + 16, "SANDBOX ", 8);
    put32(table, 24, 1);
    memcpy(table + 28, "SVMM", 4);
    put32(table, 32, 1);
}

static void finish_table(uint8_t *table, size_t length)
{
    table[9] = 0;
    table[9] = checksum(table, length);
}

/* Generic Address Structure：12 字节，描述一个 8 位系统 I/O 端口。 */
static void put_io_gas(uint8_t *table, size_t offset, uint16_t port)
{
    put8(table, offset, GAS_SYSTEM_IO);
    put8(table, offset + 1, 8);
    put8(table, offset + 2, 0);
    put8(table, offset + 3, GAS_ACCESS_BYTE);
    put64(table, offset + 4, port);
}

int boot_acpi_setup(struct guest_memory *memory)
{
    if (!memory->data || memory->size < ACPI_TABLES_END) {
        errno = EINVAL;
        return -1;
    }

    /* RSDP（ACPI 2.0 格式）：前 20 字节一个 checksum，整个 36 字节再一个。 */
    uint8_t rsdp[RSDP_SIZE] = { 0 };
    memcpy(rsdp, "RSD PTR ", 8);
    memcpy(rsdp + 9, "SVMM  ", 6);
    put8(rsdp, 15, 2);
    put32(rsdp, 16, ACPI_RSDT_ADDR);
    put32(rsdp, 20, RSDP_SIZE);
    rsdp[8] = checksum(rsdp, 20);
    rsdp[32] = checksum(rsdp, RSDP_SIZE);

    uint8_t rsdt[RSDT_SIZE] = { 0 };
    put_header(rsdt, "RSDT", RSDT_SIZE, 1);
    put32(rsdt, ACPI_HEADER_SIZE, ACPI_FADT_ADDR);
    put32(rsdt, ACPI_HEADER_SIZE + 4, ACPI_MADT_ADDR);
    finish_table(rsdt, RSDT_SIZE);

    /* FADT 6.0：字段偏移见 ACPI 规范 5.2.9 “Fixed ACPI Description Table”。 */
    uint8_t fadt[FADT_SIZE] = { 0 };
    put_header(fadt, "FACP", FADT_SIZE, 6);
    put32(fadt, 40, ACPI_DSDT_ADDR);               /* DSDT */
    put16(fadt, 109, IAPC_LEGACY_DEVICES | IAPC_VGA_NOT_PRESENT |
                     IAPC_CMOS_RTC_NOT_PRESENT);   /* IAPC_BOOT_ARCH */
    put32(fadt, 112, FADT_WBINVD | FADT_PWR_BUTTON | FADT_SLP_BUTTON |
                     FADT_RESET_REG_SUP | FADT_HW_REDUCED_ACPI);
    put_io_gas(fadt, 116, ACPI_RESET_PORT);        /* RESET_REG */
    put8(fadt, 128, ACPI_RESET_VALUE);             /* RESET_VALUE */
    put64(fadt, 140, ACPI_DSDT_ADDR);              /* X_DSDT */
    put_io_gas(fadt, 244, ACPI_SLEEP_CONTROL_PORT);
    put_io_gas(fadt, 256, ACPI_SLEEP_STATUS_PORT);
    memcpy(fadt + 268, "SVMM\0\0\0\0", 8);         /* Hypervisor Vendor Identity */
    finish_table(fadt, FADT_SIZE);

    uint8_t dsdt[DSDT_SIZE] = { 0 };
    put_header(dsdt, "DSDT", DSDT_SIZE, 2);
    memcpy(dsdt + ACPI_HEADER_SIZE, dsdt_aml, sizeof(dsdt_aml));
    finish_table(dsdt, DSDT_SIZE);

    /*
     * MADT：Local APIC 地址 + 两个条目。
     *   type 0 Processor Local APIC：ACPI 处理器 ID 0，APIC ID 0，已启用
     *   type 1 I/O APIC：ID 1，MMIO 0xFEC00000，GSI 从 0 开始
     * 没有中断源覆盖条目，ISA IRQ n 直接对应 GSI n（COM1 即 GSI 4）。
     */
    uint8_t madt[MADT_SIZE] = { 0 };
    put_header(madt, "APIC", MADT_SIZE, 4);
    put32(madt, 36, LAPIC_ADDR);
    put32(madt, 40, 0);
    size_t entry = 44;
    put8(madt, entry, 0);
    put8(madt, entry + 1, 8);
    put8(madt, entry + 2, 0);
    put8(madt, entry + 3, 0);
    put32(madt, entry + 4, 1);
    entry += 8;
    put8(madt, entry, 1);
    put8(madt, entry + 1, 12);
    put8(madt, entry + 2, 1);
    put8(madt, entry + 3, 0);
    put32(madt, entry + 4, IOAPIC_ADDR);
    put32(madt, entry + 8, 0);
    finish_table(madt, MADT_SIZE);

    if (guest_memory_load(memory, ACPI_RSDP_ADDR, rsdp, sizeof(rsdp)) < 0 ||
        guest_memory_load(memory, ACPI_RSDT_ADDR, rsdt, sizeof(rsdt)) < 0 ||
        guest_memory_load(memory, ACPI_FADT_ADDR, fadt, sizeof(fadt)) < 0 ||
        guest_memory_load(memory, ACPI_DSDT_ADDR, dsdt, sizeof(dsdt)) < 0 ||
        guest_memory_load(memory, ACPI_MADT_ADDR, madt, sizeof(madt)) < 0)
        return -1;
    return 0;
}

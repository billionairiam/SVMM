#ifndef BOOT_ACPI_H
#define BOOT_ACPI_H

#include <stdint.h>

struct guest_memory;

#define ACPI_RSDP_ADDR 0x000e0000u
#define ACPI_RSDT_ADDR 0x000e1000u
#define ACPI_FADT_ADDR 0x000e2000u
#define ACPI_DSDT_ADDR 0x000e3000u
#define ACPI_MADT_ADDR 0x000e4000u
#define ACPI_TABLES_END 0x000e5000u

#define IOAPIC_ADDR 0xfec00000u
#define LAPIC_ADDR 0xfee00000u

/*
 * FADT 声明的平台寄存器，都是 8 位 I/O 端口，由 vCPU 循环拦截：
 *   睡眠控制：写入 SLP_TYP << 2 | SLP_EN，SLP_TYP 取自 DSDT 的 _S5；
 *   睡眠状态：读恒为 0；
 *   复位：沿用 PC 上的 0xcf9 复位控制寄存器，写入 0x06 表示硬复位。
 */
#define ACPI_SLEEP_CONTROL_PORT 0x0600u
#define ACPI_SLEEP_STATUS_PORT 0x0601u
#define ACPI_SLEEP_TYPE_S5 5u
#define ACPI_SLEEP_ENABLE 0x20u
#define ACPI_RESET_PORT 0x0cf9u
#define ACPI_RESET_VALUE 0x06u

int boot_acpi_setup(struct guest_memory *memory);

#endif

use anyhow::Result;

use super::{put_u16, put_u32, put_u64};
use crate::memory::Memory;

const RSDP_ADDR: u64 = 0xe0000;
const RSDT_ADDR: u64 = 0xe1000;
const FADT_ADDR: u64 = 0xe2000;
const DSDT_ADDR: u64 = 0xe3000;
const MADT_ADDR: u64 = 0xe4000;
pub const TABLES_END: u64 = 0xe5000;

const IOAPIC_ADDR: u32 = 0xfec0_0000;
pub const LAPIC_ADDR: u32 = 0xfee0_0000;
const HEADER_SIZE: usize = 36;

// AML for _S5 and COM1 (PNP0501, ports 0x3f8–0x3ff, IRQ 4).
const DSDT_AML: &[u8] = &[
    0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12, 0x08, 0x04, 0x0a, 0x05, 0x0a, 0x05, 0x00, 0x00, 0x10, 0x32,
    0x5f, 0x53, 0x42, 0x5f, 0x5b, 0x82, 0x2b, 0x43, 0x4f, 0x4d, 0x31, 0x08, 0x5f, 0x48, 0x49, 0x44,
    0x0c, 0x41, 0xd0, 0x05, 0x01, 0x08, 0x5f, 0x55, 0x49, 0x44, 0x00, 0x08, 0x5f, 0x43, 0x52, 0x53,
    0x11, 0x10, 0x0a, 0x0d, 0x47, 0x01, 0xf8, 0x03, 0xf8, 0x03, 0x01, 0x08, 0x22, 0x10, 0x00, 0x79,
    0x00,
];

// AML for an ACPI-enumerated virtio-mmio device (LNRO0005).
const VIRTIO_AML: &[u8] = &[
    0x10, 0x43, 0x04, 0x5c, 0x5f, 0x53, 0x42, 0x5f, 0x5b, 0x82, 0x3a, 0x56, 0x52, 0x30, 0x30, 0x08,
    0x5f, 0x48, 0x49, 0x44, 0x0d, 0x4c, 0x4e, 0x52, 0x4f, 0x30, 0x30, 0x30, 0x35, 0x00, 0x08, 0x5f,
    0x55, 0x49, 0x44, 0x00, 0x08, 0x5f, 0x43, 0x52, 0x53, 0x11, 0x1a, 0x0a, 0x17, 0x86, 0x09, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xd0, 0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01, 0x01, 0x05, 0x00,
    0x00, 0x00, 0x79, 0x00,
];
const VIRTIO_NAME_DIGIT: usize = 14;
const VIRTIO_UID: usize = 35;
const VIRTIO_MEM_BASE: usize = 49;
const VIRTIO_MEM_SIZE: usize = 53;
const VIRTIO_IRQ_FLAGS: usize = 60;
const VIRTIO_GSI: usize = 62;

pub struct Tables {
    pub fadt: Vec<u8>,
    pub dsdt: Vec<u8>,
    pub madt: Vec<u8>,
}

impl Tables {
    pub fn new(vcpus: u32, disk: bool, net: bool, tdx: bool) -> Self {
        let mut fadt = vec![0; 276];
        put_u32(&mut fadt, 40, DSDT_ADDR as u32);
        // IAPC_BOOT_ARCH: legacy devices, no VGA or CMOS RTC.
        put_u16(&mut fadt, 109, (1 << 0) | (1 << 2) | (1 << 5));
        // WBINVD, power/sleep buttons, reset register, hardware-reduced ACPI.
        put_u32(
            &mut fadt,
            112,
            (1 << 0) | (1 << 4) | (1 << 5) | (1 << 10) | (1 << 20),
        );
        io_gas(&mut fadt, 116, 0xcf9);
        fadt[128] = 0x06;
        put_u64(&mut fadt, 140, DSDT_ADDR);
        io_gas(&mut fadt, 244, 0x600);
        io_gas(&mut fadt, 256, 0x601);
        fadt[268..276].copy_from_slice(b"SVMM\0\0\0\0");
        finish(&mut fadt, b"FACP", 6);

        let mut dsdt = vec![0; HEADER_SIZE];
        dsdt.extend_from_slice(DSDT_AML);
        let devices = [(disk, 0xd000_0000, 5), (net, 0xd000_1000, 6)];
        for (id, (_, address, gsi)) in devices
            .iter()
            .filter(|(present, _, _)| *present)
            .enumerate()
        {
            let offset = dsdt.len();
            dsdt.extend_from_slice(VIRTIO_AML);
            let aml = &mut dsdt[offset..];
            aml[VIRTIO_NAME_DIGIT] = b'0' + id as u8;
            aml[VIRTIO_UID] = id as u8;
            put_u32(aml, VIRTIO_MEM_BASE, *address);
            put_u32(aml, VIRTIO_MEM_SIZE, 0x1000);
            if tdx {
                aml[VIRTIO_IRQ_FLAGS] |= 2;
            }
            put_u32(aml, VIRTIO_GSI, *gsi);
        }
        finish(&mut dsdt, b"DSDT", 2);

        let mut madt = vec![0; 44];
        put_u32(&mut madt, 36, LAPIC_ADDR);
        for id in 0..vcpus as u8 {
            madt.extend_from_slice(&[0, 8, id, id, 1, 0, 0, 0]);
        }
        let mut ioapic = [1, 12, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0];
        put_u32(&mut ioapic, 4, IOAPIC_ADDR);
        madt.extend_from_slice(&ioapic);
        finish(&mut madt, b"APIC", 4);

        Self { fadt, dsdt, madt }
    }

    pub fn stage(&self, memory: &Memory) -> Result<()> {
        let mut rsdp = [0; 36];
        rsdp[..8].copy_from_slice(b"RSD PTR ");
        rsdp[9..15].copy_from_slice(b"SVMM  ");
        rsdp[15] = 2;
        put_u32(&mut rsdp, 16, RSDT_ADDR as u32);
        put_u32(&mut rsdp, 20, 36);
        rsdp[8] = checksum(&rsdp[..20]);
        rsdp[32] = checksum(&rsdp);

        let mut rsdt = [0; HEADER_SIZE + 8];
        put_u32(&mut rsdt, HEADER_SIZE, FADT_ADDR as u32);
        put_u32(&mut rsdt, HEADER_SIZE + 4, MADT_ADDR as u32);
        finish(&mut rsdt, b"RSDT", 1);

        memory.stage(RSDP_ADDR, &rsdp)?;
        memory.stage(RSDT_ADDR, &rsdt)?;
        memory.stage(FADT_ADDR, &self.fadt)?;
        memory.stage(DSDT_ADDR, &self.dsdt)?;
        memory.stage(MADT_ADDR, &self.madt)?;
        Ok(())
    }
}

fn checksum(bytes: &[u8]) -> u8 {
    0u8.wrapping_sub(bytes.iter().copied().fold(0, u8::wrapping_add))
}

fn finish(table: &mut [u8], signature: &[u8; 4], revision: u8) {
    let length = table.len() as u32;
    table[..4].copy_from_slice(signature);
    put_u32(table, 4, length);
    table[8] = revision;
    table[10..16].copy_from_slice(b"SVMM  ");
    table[16..24].copy_from_slice(b"SANDBOX ");
    put_u32(table, 24, 1);
    table[28..32].copy_from_slice(b"SVMM");
    put_u32(table, 32, 1);
    table[9] = checksum(table);
}

fn io_gas(table: &mut [u8], offset: usize, port: u16) {
    table[offset..offset + 4].copy_from_slice(&[1, 8, 0, 1]);
    put_u64(table, offset + 4, u64::from(port));
}

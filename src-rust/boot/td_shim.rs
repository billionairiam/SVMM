use std::path::Path;

use anyhow::{Context, Result, ensure};

use super::acpi::{LAPIC_ADDR, TABLES_END, Tables};
use super::linux::{Image, Initrd};
use super::{align_up, get_u32, get_u64, put_u16, put_u32, put_u64, read_image};
use crate::memory::{HIGH_RAM_BASE, LOW_RAM_LIMIT, MIB, Mapping, Memory, PAGE_SIZE};

const MAX_SECTIONS: usize = 32;
const MAX_FIRMWARE: u64 = 64 * MIB;
const FIRMWARE_MIN_GPA: u64 = LAPIC_ADDR as u64 + PAGE_SIZE;
// TD-Shim boot_kernel reserves ACPI, page tables, mailbox and event log at top of low RAM.
const RUNTIME_RESERVE: u64 = 0x10_0000 + 0x2_0000 + 0x2000 + 0x10_0000;
const METADATA_GUID: [u8; 16] = [
    0xf3, 0xf9, 0xea, 0xe9, 0x8e, 0x16, 0xd5, 0x44, 0xa8, 0xeb, 0x7f, 0x4d, 0x87, 0x38, 0xf6, 0xae,
];
const PAYLOAD_GUID: [u8; 16] = [
    0x12, 0xa4, 0x6f, 0xb9, 0x1f, 0x46, 0xe3, 0x4b, 0x8c, 0x0d, 0xad, 0x80, 0x5a, 0x49, 0x7a, 0xc0,
];
const ACPI_GUID: [u8; 16] = [
    0x70, 0x58, 0x0c, 0x6a, 0xed, 0xd4, 0xf4, 0x44, 0xa1, 0x35, 0xdd, 0x23, 0x8b, 0x6f, 0x0c, 0x8d,
];

#[derive(Clone, Copy)]
struct Section {
    raw_offset: u64,
    raw_size: u64,
    address: u64,
    size: u64,
    kind: u32,
    flags: u32,
}

const BFV: u32 = 0;
const CFV: u32 = 1;
const TD_HOB: u32 = 2;
const TEMP_MEM: u32 = 3;
const PAYLOAD: u32 = 5;
const PAYLOAD_PARAM: u32 = 6;

pub struct Shim {
    data: Vec<u8>,
    sections: Vec<Section>,
    hob: Section,
    payload: Section,
    parameter: Section,
    firmware_base: u64,
}

pub struct Staged {
    pub hob: u64,
    pub measured: [(u64, u64); 3],
    pub firmware: Mapping,
}

impl Shim {
    pub fn open(path: &Path) -> Result<Self> {
        let data = read_image(path, "TD-Shim", PAGE_SIZE, MAX_FIRMWARE)?;
        Self::parse(data).with_context(|| {
            format!(
                "invalid or unsupported TD-Shim metadata: {}",
                path.display()
            )
        })
    }

    fn parse(data: Vec<u8>) -> Result<Self> {
        let trailer = data.len() - 32;
        let offset = get_u32(&data, trailer) as usize;
        ensure!(
            offset >= METADATA_GUID.len() && offset <= trailer - 16,
            "invalid metadata offset"
        );
        ensure!(
            data[offset - METADATA_GUID.len()..offset] == METADATA_GUID
                && &data[offset..offset + 4] == b"TDVF"
                && get_u32(&data, offset + 8) == 1,
            "invalid TDVF descriptor"
        );
        let length = get_u32(&data, offset + 4) as usize;
        let count = get_u32(&data, offset + 12) as usize;
        ensure!(
            (1..=MAX_SECTIONS).contains(&count)
                && length == 16 + count * 32
                && length <= trailer - offset,
            "invalid section count or descriptor length"
        );

        let mut sections: Vec<Section> = Vec::with_capacity(count);
        let mut hob = None;
        let mut payload = None;
        let mut parameter = None;
        let mut firmware_base = HIGH_RAM_BASE;
        let mut reset_vector = false;
        for bytes in data[offset + 16..offset + length].chunks_exact(32) {
            let section = Section {
                raw_offset: u64::from(get_u32(bytes, 0)),
                raw_size: u64::from(get_u32(bytes, 4)),
                address: get_u64(bytes, 8),
                size: get_u64(bytes, 16),
                kind: get_u32(bytes, 24),
                flags: get_u32(bytes, 28),
            };
            ensure!(
                section.size != 0
                    && (section.address | section.size) % PAGE_SIZE == 0
                    && section.raw_size <= section.size
                    && section.raw_offset + section.raw_size <= data.len() as u64
                    && within(section.address, section.size, HIGH_RAM_BASE)
                    && section.kind <= PAYLOAD_PARAM
                    && section.flags & !1 == 0
                    && (section.raw_size != 0 || section.raw_offset == 0),
                "invalid section range or flags"
            );
            ensure!(
                !sections
                    .iter()
                    .any(|other| section.address < other.address + other.size
                        && other.address < section.address + section.size),
                "overlapping firmware sections"
            );
            if section.address >= FIRMWARE_MIN_GPA {
                match section.kind {
                    BFV => {
                        ensure!(
                            !reset_vector
                                && section.raw_size >= PAGE_SIZE
                                && section.flags == 1
                                && section.address + section.raw_size == HIGH_RAM_BASE
                                && section.raw_offset + section.raw_size == data.len() as u64,
                            "invalid measured boot firmware volume"
                        );
                        reset_vector = true;
                    }
                    CFV => ensure!(
                        section.raw_size != 0 && section.flags == 0,
                        "invalid config firmware volume"
                    ),
                    TEMP_MEM => ensure!(
                        section.raw_size == 0 && section.flags == 0,
                        "invalid temporary memory"
                    ),
                    _ => anyhow::bail!("unsupported high firmware section"),
                }
                firmware_base = firmware_base.min(section.address);
            } else {
                ensure!(
                    section.raw_size == 0
                        && section.address >= TABLES_END
                        && within(section.address, section.size, LOW_RAM_LIMIT),
                    "invalid low firmware section"
                );
                // The boot_kernel layout provides host-supplied payload and parameters.
                let target = match section.kind {
                    TD_HOB if section.flags == 0 => &mut hob,
                    PAYLOAD => &mut payload,
                    PAYLOAD_PARAM if section.flags == 0 => &mut parameter,
                    _ => anyhow::bail!("unsupported low firmware section"),
                };
                ensure!(
                    target.replace(section).is_none(),
                    "duplicate low firmware section"
                );
            }
            sections.push(section);
        }
        let hob = hob.context("missing HOB section")?;
        let payload = payload.context("missing payload section")?;
        let parameter = parameter.context("missing payload parameter section")?;
        ensure!(
            reset_vector
                && HIGH_RAM_BASE - firmware_base <= MAX_FIRMWARE
                && hob.address < parameter.address
                && parameter.address < payload.address,
            "unsupported firmware layout"
        );
        Ok(Self {
            data,
            sections,
            hob,
            payload,
            parameter,
            firmware_base,
        })
    }

    pub fn stage(
        &self,
        memory: &Memory,
        acpi: &Tables,
        image: &Image,
        initrd: &Initrd,
        cmdline: &str,
    ) -> Result<Staged> {
        let kernel = image.runtime_start;
        let stub_end = self.payload.address + PAGE_SIZE;
        let initrd_size = align_up(initrd.size, PAGE_SIZE);
        let limit = memory.low.size - RUNTIME_RESERVE;
        ensure!(
            within(kernel, image.init_size, limit)
                && (initrd_size == 0 || within(initrd.address, initrd_size, limit)),
            "kernel or initrd overlaps the TD-Shim runtime reservation"
        );
        let payload = image.payload();
        let payload_pages = align_up(payload.len() as u64, PAGE_SIZE);
        ensure!(
            image.is_64_bit()
                && kernel >= stub_end
                && kernel % PAGE_SIZE == 0
                && payload.len() > 0x200
                && within(self.payload.address, self.payload.size, limit)
                && within(kernel, payload_pages, limit)
                && (initrd_size == 0
                    || (initrd.address >= kernel + image.init_size
                        && initrd.address >= kernel + payload_pages
                        && initrd.address % PAGE_SIZE == 0))
                && (cmdline.len() as u64) < self.parameter.size
                && image.accepts_cmdline(cmdline),
            "incompatible TD-Shim, bzImage or initrd layout"
        );

        let firmware = Mapping::new(self.firmware_base, HIGH_RAM_BASE - self.firmware_base, None)?;
        for section in self.sections.iter().filter(|section| section.raw_size != 0) {
            let start = section.raw_offset as usize;
            let end = start + section.raw_size as usize;
            firmware.write(section.address, &self.data[start..end])?;
        }
        let header = image.header(kernel, self.parameter.address, initrd);
        memory.stage(self.parameter.address, cmdline.as_bytes())?;
        memory.stage(self.parameter.address + cmdline.len() as u64, &[0])?;
        memory.stage(kernel, payload)?;
        self.stage_linux_stub(memory, &header, kernel as u32 + 0x200)?;
        self.stage_hob(memory, acpi)?;

        Ok(Staged {
            hob: self.hob.address,
            measured: [
                (self.hob.address, stub_end - self.hob.address),
                (kernel, payload_pages),
                (initrd.address, initrd_size),
            ],
            firmware,
        })
    }

    fn stage_linux_stub(&self, memory: &Memory, header: &[u8], entry: u32) -> Result<()> {
        // TD-Shim provides the boot_params pointer in RSI. Replace its setup header
        // while keeping its e820 and ACPI data, then enter bzImage startup_64.
        let mut stub = [
            0xfc, // cld
            0x56, // push rsi
            0x48, 0x8d, 0xbe, 0xf1, 0x01, 0, 0, // lea rdi, [rsi+0x1f1]
            0x48, 0x8d, 0x35, 0, 0, 0, 0, // lea rsi, [rip+header]
            0xb9, 0, 0, 0, 0, // mov ecx, sizeof(header)
            0xf3, 0xa4, // rep movsb
            0x5e, // pop rsi
            0xb8, 0, 0, 0, 0, // mov eax, startup_64
            0xff, 0xe0, // jmp rax
        ];
        let header_displacement = (stub.len() - 16) as u32;
        put_u32(&mut stub, 12, header_displacement);
        put_u32(&mut stub, 17, header.len() as u32);
        put_u32(&mut stub, 25, entry);
        memory.stage(self.payload.address, &stub)?;
        memory.stage(self.payload.address + stub.len() as u64, header)?;
        Ok(())
    }

    fn stage_hob(&self, memory: &Memory, acpi: &Tables) -> Result<()> {
        // EFI Handoff Information Table, resource descriptors and GUID extensions.
        let mut hob = vec![0; 56];
        set_hob_header(&mut hob, 1); // EFI_HOB_TYPE_HANDOFF
        put_u32(&mut hob, 8, 9); // Version.
        append_ram_hob(&mut hob, &memory.low);
        if let Some(high) = &memory.high {
            append_ram_hob(&mut hob, high);
        }

        let mut payload = [0; 16];
        put_u32(&mut payload, 0, 2); // TD-Shim RawVmLinux.
        put_u64(&mut payload, 8, self.payload.address);
        append_guid_hob(&mut hob, &PAYLOAD_GUID, &payload);

        let mut fadt = acpi.fadt.clone();
        // TD-Shim relocates the 32-bit DSDT pointer and recomputes the checksum.
        fadt[140..148].fill(0);
        for table in [&fadt, &acpi.dsdt, &acpi.madt] {
            append_guid_hob(&mut hob, &ACPI_GUID, table);
        }

        let mut end = [0; 8];
        set_hob_header(&mut end, 0xffff);
        hob.extend_from_slice(&end);
        let end = self.hob.address + hob.len() as u64;
        put_u64(&mut hob, 48, end);
        ensure!(
            hob.len() as u64 <= self.hob.size,
            "TD-Shim HOB exceeds reservation"
        );
        memory.stage(self.hob.address, &hob)
    }
}

fn within(start: u64, size: u64, limit: u64) -> bool {
    start <= limit && size <= limit - start
}

fn set_hob_header(bytes: &mut [u8], kind: u16) {
    let length = bytes.len() as u16;
    put_u16(bytes, 0, kind);
    put_u16(bytes, 2, length);
}

fn append_ram_hob(hob: &mut Vec<u8>, map: &Mapping) {
    let mut resource = [0; 48];
    set_hob_header(&mut resource, 3); // EFI_HOB_TYPE_RESOURCE_DESCRIPTOR
    put_u32(&mut resource, 28, 7); // Present, initialized and tested.
    put_u64(&mut resource, 32, map.gpa);
    put_u64(&mut resource, 40, map.size);
    hob.extend_from_slice(&resource);
}

fn append_guid_hob(hob: &mut Vec<u8>, guid: &[u8; 16], payload: &[u8]) {
    let start = hob.len();
    hob.resize(start + 24 + payload.len(), 0);
    let entry = &mut hob[start..];
    set_hob_header(entry, 4); // EFI_HOB_TYPE_GUID_EXTENSION
    entry[8..24].copy_from_slice(guid);
    entry[24..].copy_from_slice(payload);
    hob.resize(hob.len().next_multiple_of(8), 0);
}

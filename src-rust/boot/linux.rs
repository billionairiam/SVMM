use std::path::Path;

use anyhow::{Context, Result, ensure};

use super::{
    align_up, get_u16, get_u32, get_u64, open_image, put_u16, put_u32, put_u64, read_image,
};
use crate::memory::{MIB, Memory};

pub const KERNEL_ADDR: u64 = MIB;
pub const GDT_ADDR: u64 = 0x500;
pub const STACK_ADDR: u64 = 0x90000;
pub const PARAMS_ADDR: u64 = 0x9000;
pub const CODE_SEGMENT: u16 = 0x10;
pub const DATA_SEGMENT: u16 = 0x18;
const SETUP_ADDR: u64 = 0x10000;
const CMDLINE_ADDR: u64 = 0x20000;

const IMAGE_MAX_SIZE: u64 = 512 * MIB;
const HEADER_OFFSET: usize = 0x1f1;
const HEADER_REQUIRED_END: usize = 0x264;
const HEADER_END: usize = 0x26c;
const HEADER_SIZE: usize = HEADER_END - HEADER_OFFSET;

pub struct Image {
    data: Vec<u8>,
    setup_size: usize,
    pub init_size: u64,
    pub runtime_start: u64,
    initrd_addr_max: u64,
}

#[derive(Default)]
pub struct Initrd {
    pub address: u64,
    pub size: u64,
}

impl Image {
    pub fn open(path: &Path) -> Result<Self> {
        let data = read_image(path, "bzImage", HEADER_REQUIRED_END as u64, IMAGE_MAX_SIZE)?;
        let setup_sectors = match data[HEADER_OFFSET] {
            0 => 4,
            count => usize::from(count),
        };
        let setup_size = (setup_sectors + 1) * 512;
        let init_size = u64::from(get_u32(&data, 0x260));
        let preferred = get_u64(&data, 0x258);
        let alignment = u64::from(get_u32(&data, 0x230));
        let relocatable = data[0x234] != 0;
        ensure!(
            &data[0x202..0x206] == b"HdrS"
                && get_u16(&data, 0x206) >= 0x020c
                && data[0x211] & 1 != 0
                && header_end(&data) >= HEADER_REQUIRED_END
                && setup_size <= (CMDLINE_ADDR - SETUP_ADDR) as usize
                && setup_size < data.len()
                && init_size <= IMAGE_MAX_SIZE
                && init_size >= (data.len() - setup_size) as u64
                && preferred <= u64::from(u32::MAX)
                && (!relocatable || alignment.is_power_of_two()),
            "unsupported or malformed x86 bzImage"
        );
        let runtime_start = if relocatable {
            align_up(preferred.max(KERNEL_ADDR), alignment)
        } else {
            preferred
        };
        ensure!(
            runtime_start + init_size <= u64::from(u32::MAX),
            "unsupported or malformed x86 bzImage"
        );
        let initrd_addr_max = u64::from(get_u32(&data, 0x22c));
        Ok(Self {
            data,
            setup_size,
            init_size,
            runtime_start,
            initrd_addr_max,
        })
    }

    pub fn load_initrd(&self, path: &Path, memory: &Memory) -> Result<Initrd> {
        let capacity = memory.low.size;
        let address = align_up(
            (capacity / 8 * 3)
                .max(16 * MIB)
                .max(self.runtime_start + self.init_size),
            2 * MIB,
        );
        let limit = (capacity - 16 * MIB).min(self.initrd_addr_max + 1);
        let (mut file, size) = open_image(path, "initramfs")?;
        ensure!(size != 0, "invalid initramfs file: {}", path.display());
        ensure!(
            address < limit && size <= limit - address,
            "initramfs ({size} bytes) does not fit in {} MiB of guest memory: space is [{address:#x}, {limit:#x}); try a larger --mem",
            capacity / MIB
        );
        memory
            .load(address, size as usize, &mut file)
            .context("read initramfs")?;
        eprintln!(
            "INFO: initramfs {} loaded at {address:#x} ({size} bytes)",
            path.display()
        );
        Ok(Initrd { address, size })
    }

    pub fn payload(&self) -> &[u8] {
        &self.data[self.setup_size..]
    }

    pub fn is_64_bit(&self) -> bool {
        get_u16(&self.data, 0x236) & 1 != 0
    }

    pub fn accepts_cmdline(&self, cmdline: &str) -> bool {
        let max = get_u32(&self.data, 0x238) as usize;
        !cmdline.contains('\0') && (max == 0 || cmdline.len() <= max)
    }

    pub fn header(&self, kernel: u64, cmdline: u64, initrd: &Initrd) -> [u8; HEADER_SIZE] {
        let mut header = [0; HEADER_SIZE];
        let end = header_end(&self.data).min(HEADER_END);
        header[..end - HEADER_OFFSET].copy_from_slice(&self.data[HEADER_OFFSET..end]);
        header[0x210 - HEADER_OFFSET] = 0xff;
        put_u32(&mut header, 0x214 - HEADER_OFFSET, kernel as u32);
        put_u32(&mut header, 0x218 - HEADER_OFFSET, initrd.address as u32);
        put_u32(&mut header, 0x21c - HEADER_OFFSET, initrd.size as u32);
        put_u32(&mut header, 0x228 - HEADER_OFFSET, cmdline as u32);
        put_u64(&mut header, 0x250 - HEADER_OFFSET, 0);
        header
    }

    pub fn stage(&self, memory: &Memory, cmdline: &str, initrd: &Initrd) -> Result<()> {
        let capacity = memory.low.size;
        let runtime_end = self.runtime_start + self.init_size;
        ensure!(
            runtime_end <= capacity,
            "the kernel decompresses into [{:#x}, {runtime_end:#x}), beyond {} MiB of guest memory; try a larger --mem",
            self.runtime_start,
            capacity / MIB
        );
        ensure!(
            self.payload().len() as u64 <= capacity - KERNEL_ADDR,
            "compressed kernel does not fit in guest memory"
        );
        ensure!(
            cmdline.len() < 4096 && self.accepts_cmdline(cmdline),
            "kernel command line is too long or contains NUL"
        );

        let mut header = self.header(KERNEL_ADDR, CMDLINE_ADDR, initrd);
        header[0x211 - HEADER_OFFSET] |= 0x80; // CAN_USE_HEAP.
        put_u16(&mut header, 0x224 - HEADER_OFFSET, 0xde00);
        let mut params = [0; 4096];
        params[HEADER_OFFSET..HEADER_END].copy_from_slice(&header);

        // e820. Reserve the first 64 KiB where the GDT and zero page reside.
        let regions = [
            (0, 0x10000, 2),
            (0x10000, 0xf0000, 1),
            (MIB, capacity - MIB, 1),
        ];
        params[0x1e8] = regions.len() as u8;
        for (index, (start, size, kind)) in regions.into_iter().enumerate() {
            let offset = 0x2d0 + 20 * index;
            put_u64(&mut params, offset, start);
            put_u64(&mut params, offset + 8, size);
            put_u32(&mut params, offset + 16, kind);
        }

        let mut gdt = [0; 32];
        put_u64(&mut gdt, usize::from(CODE_SEGMENT), 0x00cf_9b00_0000_ffff);
        put_u64(&mut gdt, usize::from(DATA_SEGMENT), 0x00cf_9300_0000_ffff);
        memory.stage(GDT_ADDR, &gdt)?;
        memory.stage(PARAMS_ADDR, &params)?;
        memory.stage(SETUP_ADDR, &self.data[..self.setup_size])?;
        memory.stage(SETUP_ADDR + HEADER_OFFSET as u64, &header)?;
        memory.stage(CMDLINE_ADDR, cmdline.as_bytes())?;
        memory.stage(CMDLINE_ADDR + cmdline.len() as u64, &[0])?;
        memory.stage(KERNEL_ADDR, self.payload())?;
        Ok(())
    }
}

fn header_end(data: &[u8]) -> usize {
    // The jump at 0x200 skips the header. Its displacement is relative to 0x202.
    0x202 + usize::from(data[0x201])
}

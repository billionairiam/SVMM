use anyhow::{Context, Result};
use kvm_bindings::kvm_msi;
use kvm_ioctls::VmFd;

pub const BASE: u64 = 0xfec0_0000;
const PINS: usize = 24;
const MASKED: u64 = 1 << 16;
const LEVEL_TRIGGERED: u64 = 1 << 15;

pub struct Ioapic {
    select: u8,
    id: u32,
    high: u32,
    pending: u32,
    entries: [u64; PINS],
}

impl Ioapic {
    pub fn new() -> Self {
        Self {
            select: 0,
            id: 1,
            high: 0,
            pending: 0,
            entries: [MASKED; PINS],
        }
    }

    fn deliver(&mut self, pin: usize, vm: &VmFd) -> Result<()> {
        let entry = self.entries[pin];
        let bit = 1 << pin;
        let vector = entry as u32 & 0xff;
        let mode = (entry as u32 >> 8) & 7;
        if self.pending & bit == 0
            || entry & (MASKED | LEVEL_TRIGGERED) != 0
            || vector < 16
            || mode > 1
        {
            return Ok(());
        }
        // TDX uses a split irqchip. Keep a masked edge pending until Linux unmasks it.
        let msi = kvm_msi {
            address_lo: 0xfee0_0000
                | ((entry >> 56) as u32) << 12
                | if entry & (1 << 11) != 0 { 4 } else { 0 },
            data: vector | mode << 8,
            ..Default::default()
        };
        if vm.signal_msi(msi).context("KVM_SIGNAL_MSI")? > 0 {
            self.pending &= !bit;
        }
        Ok(())
    }

    pub fn signal(&mut self, pin: u32, level: bool, vm: &VmFd) -> Result<()> {
        let bit = 1 << pin;
        if level {
            if self.high & bit == 0 {
                self.pending |= bit;
            }
            self.high |= bit;
            self.deliver(pin as usize, vm)?;
        } else {
            self.high &= !bit;
        }
        Ok(())
    }

    pub fn access(&mut self, offset: u64, data: &mut [u8], write: bool, vm: &VmFd) -> Result<()> {
        if data.len() != 4 || (offset != 0 && offset != 0x10) {
            if !write {
                data.fill(0);
            }
            return Ok(());
        }

        if offset == 0 {
            if write {
                self.select = data[0];
            } else {
                data.copy_from_slice(&u32::from(self.select).to_le_bytes());
            }
            return Ok(());
        }

        let value = super::read_u32(data, 0);
        let result = match self.select {
            0 => {
                if write {
                    self.id = (value >> 24) & 0xf;
                }
                self.id << 24
            }
            1 => ((PINS as u32 - 1) << 16) | 0x11,
            2 => self.id << 24,
            register @ 0x10..=0x3f => {
                let pin = usize::from(register - 0x10) / 2;
                let shift = u32::from(register & 1) * 32;
                if write {
                    let mask = if shift == 0 {
                        0x0001_afff_u64
                    } else {
                        0xff00_0000
                    } << shift;
                    self.entries[pin] =
                        (self.entries[pin] & !mask) | ((u64::from(value) << shift) & mask);
                    self.deliver(pin, vm)?;
                }
                (self.entries[pin] >> shift) as u32
            }
            _ => 0,
        };
        if !write {
            data.copy_from_slice(&result.to_le_bytes());
        }
        Ok(())
    }
}

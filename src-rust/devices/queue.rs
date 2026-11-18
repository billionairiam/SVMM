use anyhow::{Result, bail, ensure};

const NEXT: u16 = 1;
const WRITE: u16 = 2;
const INDIRECT: u16 = 4;
const NO_INTERRUPT: u16 = 1;
pub const MAX_SIZE: usize = 256;

/// Access to the shared split-ring memory. The index operations publish or
/// observe a whole 16-bit field with the required ordering.
pub trait QueueMemory {
    fn check(&self, addr: u64, len: usize) -> Result<()>;
    fn read(&self, addr: u64, dst: &mut [u8]) -> Result<()>;
    fn write(&self, addr: u64, src: &[u8]) -> Result<()>;
    fn read_u16_acquire(&self, addr: u64) -> Result<u16>;
    fn write_u16_release(&self, addr: u64, value: u16) -> Result<()>;
}

#[derive(Clone, Copy, Default)]
pub struct Queue {
    pub num: u32,
    pub desc: u64,
    pub avail: u64,
    pub used: u64,
    pub ready: bool,
    pub last_seen_avail: u16,
    pub next_used_idx: u16,
}

#[derive(Clone, Copy)]
pub struct Desc {
    pub addr: u64,
    pub len: usize,
    pub writable: bool,
}

impl Queue {
    pub fn validate(&self, dma: &impl QueueMemory, max: u32) -> Result<()> {
        ensure!(
            (2..=max.min(MAX_SIZE as u32)).contains(&self.num) && self.num.is_power_of_two(),
            "invalid virtqueue size"
        );
        ensure!(
            self.desc & 15 == 0 && self.avail & 1 == 0 && self.used & 3 == 0,
            "unaligned virtqueue"
        );
        let size = self.num as usize;
        dma.check(self.desc, 16 * size)?;
        dma.check(self.avail, 6 + 2 * size)?;
        dma.check(self.used, 6 + 8 * size)?;
        Ok(())
    }

    fn slot(&self, index: u16) -> u64 {
        u64::from(index & (self.num as u16 - 1))
    }

    pub fn peek(&self, dma: &impl QueueMemory) -> Result<Option<u16>> {
        let published = dma.read_u16_acquire(self.avail + 2)?;
        let pending = published.wrapping_sub(self.last_seen_avail);
        if pending == 0 {
            return Ok(None);
        }
        ensure!(
            u32::from(pending) <= self.num,
            "invalid virtqueue avail index"
        );
        let mut bytes = [0; 2];
        dma.read(
            self.avail + 4 + 2 * self.slot(self.last_seen_avail),
            &mut bytes,
        )?;
        let head = u16::from_le_bytes(bytes);
        ensure!(u32::from(head) < self.num, "invalid virtqueue head");
        Ok(Some(head))
    }

    pub fn complete(&mut self, dma: &impl QueueMemory, head: u16, len: u32) -> Result<()> {
        let mut entry = [0; 8];
        entry[..4].copy_from_slice(&u32::from(head).to_le_bytes());
        entry[4..].copy_from_slice(&len.to_le_bytes());
        dma.write(self.used + 4 + 8 * self.slot(self.next_used_idx), &entry)?;
        let next = self.next_used_idx.wrapping_add(1);
        dma.write_u16_release(self.used + 2, next)?;
        self.next_used_idx = next;
        self.last_seen_avail = self.last_seen_avail.wrapping_add(1);
        Ok(())
    }

    pub fn needs_interrupt(&self, dma: &impl QueueMemory) -> Result<bool> {
        Ok(dma.read_u16_acquire(self.avail)? & NO_INTERRUPT == 0)
    }

    pub fn chain(&self, dma: &impl QueueMemory, head: u16) -> Result<Vec<Desc>> {
        let mut chain = Vec::new();
        let mut visited = [false; MAX_SIZE];
        let mut index = head;
        let mut writable = false;
        loop {
            ensure!(u32::from(index) < self.num, "invalid descriptor index");
            ensure!(
                !visited[usize::from(index)],
                "descriptor chain contains a cycle"
            );
            visited[usize::from(index)] = true;

            // A guest may edit the shared table concurrently. Validate and use this copy.
            let mut bytes = [0; 16];
            dma.read(self.desc + u64::from(index) * 16, &mut bytes)?;
            let flags = read_u16(&bytes, 12);
            if flags & INDIRECT != 0 {
                bail!("indirect descriptors are not supported");
            }
            let current_writable = flags & WRITE != 0;
            ensure!(
                !writable || current_writable,
                "readable descriptor follows writable descriptor"
            );
            writable |= current_writable;
            chain.push(Desc {
                addr: read_u64(&bytes, 0),
                len: read_u32(&bytes, 8) as usize,
                writable: current_writable,
            });
            if flags & NEXT == 0 {
                return Ok(chain);
            }
            index = read_u16(&bytes, 14);
        }
    }
}

pub(super) fn read_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap())
}

pub(super) fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

pub(super) fn read_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

mod queue;

use std::cell::RefCell;
use std::ops::Range;

use anyhow::{Context, Result, ensure};

use queue::{Queue, QueueMemory, read_u16, read_u32, read_u64};

const GUEST_SIZE: usize = 0x10000;
const SECTOR: usize = 512;
const DISK_SECTORS: usize = 16;
const READ_HEADER: u64 = 0x1000;
const READ_DATA: u64 = 0x2000;
const WRITE_HEADER: u64 = 0x3000;
const SEGMENTS: [u64; 3] = [0x4000, 0x5040, 0x6080];
const SEGMENT_SIZE: usize = 64;

struct GuestRam(RefCell<Vec<u8>>);

impl GuestRam {
    fn range(&self, addr: u64, len: usize) -> Result<Range<usize>> {
        let start = usize::try_from(addr)?;
        let end = start.checked_add(len).context("guest address overflows")?;
        ensure!(end <= self.0.borrow().len(), "request outside guest memory");
        Ok(start..end)
    }

    fn bytes<const N: usize>(&self, addr: u64) -> Result<[u8; N]> {
        let mut data = [0; N];
        self.read(addr, &mut data)?;
        Ok(data)
    }
}

impl QueueMemory for GuestRam {
    fn check(&self, addr: u64, len: usize) -> Result<()> {
        self.range(addr, len).map(|_| ())
    }

    fn read(&self, addr: u64, dst: &mut [u8]) -> Result<()> {
        let range = self.range(addr, dst.len())?;
        dst.copy_from_slice(&self.0.borrow()[range]);
        Ok(())
    }

    fn write(&self, addr: u64, src: &[u8]) -> Result<()> {
        let range = self.range(addr, src.len())?;
        self.0.borrow_mut()[range].copy_from_slice(src);
        Ok(())
    }

    // Both sides of this demo run synchronously on one thread.
    fn read_u16_acquire(&self, addr: u64) -> Result<u16> {
        Ok(u16::from_le_bytes(self.bytes::<2>(addr)?))
    }

    fn write_u16_release(&self, addr: u64, value: u16) -> Result<()> {
        self.write(addr, &value.to_le_bytes())
    }
}

struct Demo {
    ram: GuestRam,
    queue: Queue,
    disk: [u8; SECTOR * DISK_SECTORS],
    reclaimed: u16,
    doorbells: u64,
    interrupts: u64,
}

impl Demo {
    fn new() -> Result<Self> {
        let ram = GuestRam(RefCell::new(vec![0; GUEST_SIZE]));
        let queue = Queue {
            num: 8,
            desc: 0x100,
            avail: 0x200,
            used: 0x300,
            ready: true,
            ..Queue::default()
        };
        queue.validate(&ram, 8)?;

        let mut disk = [0; SECTOR * DISK_SECTORS];
        for (number, sector) in disk.chunks_exact_mut(SECTOR).enumerate() {
            let pattern = format!("<disk sector {number}>");
            for (slot, byte) in sector.iter_mut().zip(pattern.bytes().cycle()) {
                *slot = byte;
            }
        }
        Ok(Self {
            ram,
            queue,
            disk,
            reclaimed: 0,
            doorbells: 0,
            interrupts: 0,
        })
    }

    fn set_header(&self, addr: u64, kind: u32, sector: u64) -> Result<()> {
        let mut header = [0; 16];
        header[..4].copy_from_slice(&kind.to_le_bytes());
        header[8..].copy_from_slice(&sector.to_le_bytes());
        self.ram.write(addr, &header)
    }

    fn set_desc(&self, index: u16, addr: u64, len: u32, flags: u16, next: u16) -> Result<()> {
        let mut desc = [0; 16];
        desc[..8].copy_from_slice(&addr.to_le_bytes());
        desc[8..12].copy_from_slice(&len.to_le_bytes());
        desc[12..14].copy_from_slice(&flags.to_le_bytes());
        desc[14..16].copy_from_slice(&next.to_le_bytes());
        self.ram
            .write(self.queue.desc + 16 * u64::from(index), &desc)
    }

    fn dump(&self, label: &str) -> Result<()> {
        println!("\n--- virtq state: {label} ---");
        println!("--- Descriptor Table (qsz={}) ---", self.queue.num);
        println!("Index Address            Length Flags Next");
        for index in 0..self.queue.num {
            let descriptor = self
                .ram
                .bytes::<16>(self.queue.desc + 16 * u64::from(index))?;
            let flags = match read_u16(&descriptor, 12) {
                1 => "NEXT",
                2 => "WRITE",
                _ => "-",
            };
            println!(
                "{index:<5} 0x{:016x} {:>6} {flags:<5} {:>4}",
                read_u64(&descriptor, 0),
                read_u32(&descriptor, 8),
                read_u16(&descriptor, 14)
            );
        }
        let available = self.ram.read_u16_acquire(self.queue.avail + 2)?;
        let used = self.ram.read_u16_acquire(self.queue.used + 2)?;
        println!("avail.idx = {available}");
        println!(
            "avail.ring[] (device view: last_seen={}, count={})",
            self.queue.last_seen_avail,
            available.wrapping_sub(self.queue.last_seen_avail)
        );
        println!("used.idx = {used}");
        println!(
            "used.ring[] (driver view: last_used={}, count={})\n",
            self.reclaimed,
            used.wrapping_sub(self.reclaimed)
        );
        Ok(())
    }

    fn submit(&mut self) -> Result<()> {
        ensure!(self.queue.ready, "demo queue is unavailable");
        const HEAD: u16 = 0;
        let published = self.ram.read_u16_acquire(self.queue.avail + 2)?;
        ensure!(
            u32::from(published.wrapping_sub(self.reclaimed)) < self.queue.num,
            "avail ring full"
        );
        let slot = u64::from(published) % u64::from(self.queue.num);
        self.ram
            .write(self.queue.avail + 4 + 2 * slot, &HEAD.to_le_bytes())?;
        let published = published.wrapping_add(1);
        self.ram
            .write_u16_release(self.queue.avail + 2, published)?;
        println!("driver: avail.ring[{slot}] = {HEAD}, avail.idx = {published}");
        self.dump("after driver submit")?;

        self.doorbells += 1;
        println!(
            "device: doorbell #{} (in a real VMM: MMIO write -> VM exit)",
            self.doorbells
        );
        while let Some(head) = self.queue.peek(&self.ram)? {
            let written = self.transfer(head)?;
            self.queue.complete(&self.ram, head, written)?;
            println!(
                "device: used.ring <- {{id={head}, len={written}}}, used.idx = {}",
                self.queue.next_used_idx
            );
        }
        if self.queue.needs_interrupt(&self.ram)? {
            self.interrupts += 1;
            println!(
                "IRQ: used-buffer interrupt #{} (used.idx={})",
                self.interrupts, self.queue.next_used_idx
            );
        }
        self.dump("after device completes")
    }

    fn transfer(&mut self, head: u16) -> Result<u32> {
        let chain = self.queue.chain(&self.ram, head)?;
        let readable: usize = chain.iter().filter(|d| !d.writable).map(|d| d.len).sum();
        let writable: usize = chain.iter().filter(|d| d.writable).map(|d| d.len).sum();
        println!(
            "chain head {head}: {} descriptors, {readable} bytes device-readable, {writable} bytes device-writable",
            chain.len()
        );

        let header = &chain[0];
        ensure!(
            !header.writable && header.len >= 16,
            "invalid demo request header"
        );
        let bytes = self.ram.bytes::<16>(header.addr)?;
        let kind = read_u32(&bytes, 0);
        let sector = usize::try_from(read_u64(&bytes, 8))?;
        ensure!(
            matches!(kind, 0 | 1) && sector < DISK_SECTORS,
            "invalid demo request"
        );
        let mut offset = sector * SECTOR;
        for desc in &chain[1..] {
            ensure!(
                desc.writable == (kind == 0),
                "invalid demo descriptor direction"
            );
            let next = offset
                .checked_add(desc.len)
                .context("request length overflow")?;
            let disk = self
                .disk
                .get_mut(offset..next)
                .context("request outside demo disk")?;
            if desc.writable {
                self.ram.write(desc.addr, disk)?;
            } else {
                self.ram.read(desc.addr, disk)?;
            }
            offset = next;
        }
        Ok(writable.try_into()?)
    }

    fn reclaim(&mut self) -> Result<u32> {
        let idx = self.ram.read_u16_acquire(self.queue.used + 2)?;
        ensure!(
            idx.wrapping_sub(self.reclaimed) == 1,
            "expected one demo completion"
        );
        let slot = u64::from(self.reclaimed) % u64::from(self.queue.num);
        let elem = self.ram.bytes::<8>(self.queue.used + 4 + 8 * slot)?;
        let head = read_u32(&elem, 0);
        let len = read_u32(&elem, 4);
        ensure!(head < self.queue.num, "invalid demo completion head");
        self.reclaimed = self.reclaimed.wrapping_add(1);
        println!("driver: used elem {{id={head}, len={len}}}; chain {head} can be reused");
        Ok(len)
    }

    fn read_scenario(&mut self) -> Result<()> {
        println!("=== Scenario 1: virtio-blk read, sector 3 (header RO + data WO) ===");
        self.set_header(READ_HEADER, 0, 3)?;
        self.set_desc(0, READ_HEADER, 16, 1, 1)?;
        self.set_desc(1, READ_DATA, SECTOR as u32, 2, 0)?;
        self.submit()?;

        let len = self.reclaim()?;
        let bytes = self.ram.bytes::<64>(READ_DATA)?;
        println!(
            "driver: data buffer @{READ_DATA:#x} = \"{}\"",
            String::from_utf8_lossy(&bytes[..(len as usize).min(bytes.len())])
        );
        self.dump("after driver reclaims")
    }

    fn write_scenario(&mut self) -> Result<()> {
        println!("=== Scenario 2: scatter/gather write, sector 5 (header + 3 segments) ===");
        self.set_header(WRITE_HEADER, 1, 5)?;
        self.set_desc(0, WRITE_HEADER, 16, 1, 1)?;
        for (index, addr) in SEGMENTS.into_iter().enumerate() {
            self.ram.write(addr, &[b'A' + index as u8; SEGMENT_SIZE])?;
            let desc = index as u16 + 1;
            let next = if index + 1 < SEGMENTS.len() {
                desc + 1
            } else {
                0
            };
            self.set_desc(desc, addr, SEGMENT_SIZE as u32, u16::from(next != 0), next)?;
        }
        self.submit()?;

        self.reclaim()?;
        println!(
            "host: disk sector 5 [0..256) = \"{}\"",
            String::from_utf8_lossy(&self.disk[5 * SECTOR..5 * SECTOR + 256])
        );
        self.dump("after driver reclaims")
    }
}

fn main() -> Result<()> {
    let mut demo = Demo::new()?;
    println!(
        "virtio-blk: 1 split virtqueue, qsz={}, guest RAM {} KiB (simulated, no KVM)\n",
        demo.queue.num,
        GUEST_SIZE / 1024
    );
    demo.read_scenario()?;
    demo.write_scenario()?;
    println!(
        "summary: {} doorbells, {} interrupts, avail.idx={}, used.idx={}",
        demo.doorbells,
        demo.interrupts,
        demo.ram.read_u16_acquire(demo.queue.avail + 2)?,
        demo.ram.read_u16_acquire(demo.queue.used + 2)?
    );
    Ok(())
}

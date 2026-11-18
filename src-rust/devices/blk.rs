use std::fs::{File, OpenOptions};
use std::io;
use std::os::fd::AsRawFd;
use std::os::unix::fs::{FileExt, FileTypeExt};
use std::path::Path;

use anyhow::{Context, Result, bail, ensure};

use crate::memory::Dma;

use super::queue::{Desc, Queue};
use super::transport::{Backend, Stats as TransportStats};

pub const BASE: u64 = 0xd000_0000;
const SECTOR_SIZE: u64 = 512;
const BLKGETSIZE64: libc::Ioctl = libc::_IOR::<u64>(0x12, 114);
const ID: [u8; 20] = *b"svmm-vda\0\0\0\0\0\0\0\0\0\0\0\0";
const IN: u32 = 0;
const OUT: u32 = 1;
const FLUSH: u32 = 4;
const GET_ID: u32 = 8;
const OK: u8 = 0;
const IOERR: u8 = 1;
const UNSUPPORTED: u8 = 2;

#[derive(Default)]
struct Stats {
    requests: u64,
    reads: u64,
    writes: u64,
    flushes: u64,
    errors: u64,
    bytes_read: u64,
    bytes_written: u64,
}

pub struct Block {
    pub file: File,
    capacity: u64,
    config: [u8; 24],
    stats: Stats,
}

impl Block {
    pub fn open(path: &Path) -> Result<Self> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)
            .with_context(|| format!("open disk image {}", path.display()))?;
        let metadata = file.metadata()?;
        let bytes = if metadata.is_file() {
            metadata.len()
        } else if metadata.file_type().is_block_device() {
            let mut size = 0u64;
            // BLKGETSIZE64 writes one Linux u64 to this pointer.
            if unsafe { libc::ioctl(file.as_raw_fd(), BLKGETSIZE64, &mut size) } < 0 {
                return Err(io::Error::last_os_error())
                    .with_context(|| format!("size of block device {}", path.display()));
            }
            size
        } else {
            bail!(
                "disk image {} is not a regular file or block device",
                path.display()
            );
        };

        // The lock belongs to this open file description and survives try_clone().
        if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } < 0 {
            let error = io::Error::last_os_error();
            if error.kind() == io::ErrorKind::WouldBlock {
                bail!("disk image {} is in use by another process", path.display());
            }
            return Err(error).with_context(|| format!("lock disk image {}", path.display()));
        }
        let capacity = bytes / SECTOR_SIZE;
        ensure!(
            capacity > 0,
            "disk image {} is smaller than one {SECTOR_SIZE}-byte sector",
            path.display()
        );

        let mut config = [0; 24];
        config[..8].copy_from_slice(&capacity.to_le_bytes());
        config[12..16].copy_from_slice(&(Self::QUEUE_SIZE - 2).to_le_bytes());
        config[20..24].copy_from_slice(&(SECTOR_SIZE as u32).to_le_bytes());
        Ok(Self {
            file,
            capacity,
            config,
            stats: Stats::default(),
        })
    }

    pub fn print_info(&self, path: &Path) {
        eprintln!(
            "INFO: virtio-blk {}: {} sectors, mmio {BASE:#x} irq {}",
            path.display(),
            self.capacity,
            Self::IRQ
        );
    }

    pub fn print_stats(&self, transport: &TransportStats) {
        let s = &self.stats;
        eprintln!(
            "virtio-blk: requests={} reads={} writes={} flushes={} errors={} bytes_read={} bytes_written={} notifies={} interrupts={}",
            s.requests,
            s.reads,
            s.writes,
            s.flushes,
            s.errors,
            s.bytes_read,
            s.bytes_written,
            transport.notifies,
            transport.interrupts
        );
    }

    fn request_error(&mut self, error: &anyhow::Error) {
        self.stats.errors += 1;
        if self.stats.errors < 16 {
            eprintln!("virtio-blk: request rejected: {error:#}");
        } else if self.stats.errors == 16 {
            eprintln!("virtio-blk: further request errors are not logged");
        }
    }

    fn request(&mut self, queue: &Queue, dma: &Dma<'_>, head: u16) -> u32 {
        self.stats.requests += 1;
        let chain = queue.chain(dma, head).and_then(|chain| {
            ensure!(chain.len() >= 2, "no writable status byte");
            Ok(chain)
        });
        let status_address = chain.and_then(|chain| {
            let last = chain.last().unwrap();
            ensure!(last.writable && last.len > 0, "no writable status byte");
            let pos = last
                .addr
                .checked_add(last.len as u64 - 1)
                .context("status address overflows")?;
            dma.check(pos, 1)?;
            Ok((chain, pos))
        });
        let (chain, status_address) = match status_address {
            Ok(found) => found,
            Err(error) => {
                self.request_error(&error);
                return 0;
            }
        };

        let mut written = 0;
        let status = match self.process(&chain, dma, &mut written) {
            Ok(status) => status,
            Err(error) => {
                self.request_error(&error);
                IOERR
            }
        };
        if let Err(error) = dma.write(status_address, &[status]) {
            self.request_error(&error);
            return written;
        }
        written.saturating_add(1)
    }

    fn process(&mut self, chain: &[Desc], dma: &Dma<'_>, written: &mut u32) -> Result<u8> {
        let mut header = [0; 16];
        ensure!(
            !chain[0].writable && chain[0].len >= header.len(),
            "bad request header"
        );
        dma.read(chain[0].addr, &mut header)?;
        let segments = &chain[1..chain.len() - 1];

        match super::read_u32(&header, 0) {
            request @ (IN | OUT) => {
                self.transfer(
                    segments,
                    dma,
                    request == IN,
                    super::read_u64(&header, 8),
                    written,
                )?;
            }
            FLUSH => {
                self.stats.flushes += 1;
                self.file.sync_data().context("flush disk image")?;
            }
            GET_ID => {
                let segment = segments
                    .first()
                    .filter(|d| d.writable)
                    .context("GET_ID needs a device-writable buffer")?;
                let len = segment.len.min(ID.len());
                dma.write(segment.addr, &ID[..len])?;
                *written = len as u32;
            }
            unsupported => {
                self.request_error(&anyhow::anyhow!("unsupported request type {unsupported}"));
                return Ok(UNSUPPORTED);
            }
        }
        Ok(OK)
    }

    fn transfer(
        &mut self,
        segments: &[Desc],
        dma: &Dma<'_>,
        read: bool,
        sector: u64,
        written: &mut u32,
    ) -> Result<()> {
        // Validate the entire request before changing the disk or its data buffers.
        let mut total = 0;
        for desc in segments {
            ensure!(desc.writable == read, "invalid block buffer direction");
            dma.check(desc.addr, desc.len)?;
            total += desc.len as u64;
        }
        ensure!(
            total > 0 && total % SECTOR_SIZE == 0,
            "invalid block transfer length {total}"
        );
        ensure!(
            sector <= self.capacity && total / SECTOR_SIZE <= self.capacity - sector,
            "block transfer exceeds disk capacity"
        );

        let mut offset = sector * SECTOR_SIZE;
        let mut buffer = [0; 64 * 1024];
        for desc in segments {
            let mut done = 0;
            while done < desc.len {
                let count = buffer.len().min(desc.len - done);
                let bytes = &mut buffer[..count];
                if read {
                    self.file
                        .read_exact_at(bytes, offset)
                        .context("read disk image")?;
                    dma.write(desc.addr + done as u64, bytes)?;
                    *written = written.saturating_add(count as u32);
                } else {
                    dma.read(desc.addr + done as u64, bytes)?;
                    self.file
                        .write_all_at(bytes, offset)
                        .context("write disk image")?;
                }
                done += count;
                offset += count as u64;
            }
        }
        if read {
            self.stats.reads += 1;
            self.stats.bytes_read += total;
        } else {
            self.stats.writes += 1;
            self.stats.bytes_written += total;
        }
        Ok(())
    }
}

impl Backend for Block {
    const DEVICE_ID: u32 = 2;
    const IRQ: u32 = 5;
    const FEATURES: u64 = (1 << 2) | (1 << 6) | (1 << 9); // SEG_MAX, BLK_SIZE, FLUSH
    const QUEUES: usize = 1;
    const QUEUE_SIZE: u32 = 128;

    fn config(&self) -> &[u8] {
        &self.config
    }

    fn notify(&mut self, _index: usize, queue: &mut Queue, dma: &Dma<'_>) -> Result<()> {
        // Bound a kick even when a guest keeps publishing requests while we process it.
        for _ in 0..queue.num {
            let Some(head) = queue.peek(dma)? else {
                break;
            };
            let len = self.request(queue, dma, head);
            queue.complete(dma, head, len)?;
        }
        Ok(())
    }
}

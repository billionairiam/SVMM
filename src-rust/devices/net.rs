use std::ffi::CString;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, RawFd};
use std::os::unix::fs::OpenOptionsExt;

use anyhow::{Context, Result, bail, ensure};

use crate::memory::Dma;

use super::queue::{Desc, Queue};
use super::transport::{Backend, Stats as TransportStats};

pub const BASE: u64 = 0xd000_1000;
const MAC: [u8; 6] = [0x52, 0x54, 0, 0x12, 0x34, 0x56];
const HEADER: usize = 12;
const ETH_HEADER: usize = 14;
const MAX_FRAME: usize = 65535 + ETH_HEADER + 4;
const RX: usize = 0;

#[derive(Default)]
struct Stats {
    tx_packets: u64,
    tx_bytes: u64,
    tx_errors: u64,
    rx_packets: u64,
    rx_bytes: u64,
    rx_dropped: u64,
    bad_chains: u64,
}

pub struct Net {
    tap: File,
    wake: File,
    stats: Stats,
    logged_errors: u8,
    tap_broken: bool,
    rx_frame: Vec<u8>,
    rx_len: usize,
}

impl Net {
    pub fn open(name: &str) -> Result<Self> {
        ensure!(!name.is_empty(), "tap name is empty");
        ensure!(name.len() < libc::IFNAMSIZ, "tap name '{name}' is too long");
        let c_name = CString::new(name).context("invalid tap name")?;
        // TUNSETIFF would implicitly create a missing TAP.
        ensure!(
            unsafe { libc::if_nametoindex(c_name.as_ptr()) } != 0,
            "tap {name} does not exist"
        );
        let tap = OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(libc::O_NONBLOCK)
            .open("/dev/net/tun")
            .context("open /dev/net/tun")?;
        // Linux ifreq accepts the TAP name and flags with the other bytes zeroed.
        let mut req: libc::ifreq = unsafe { std::mem::zeroed() };
        for (slot, value) in req.ifr_name.iter_mut().zip(c_name.as_bytes_with_nul()) {
            *slot = *value as libc::c_char;
        }
        req.ifr_ifru.ifru_flags = (libc::IFF_TAP | libc::IFF_NO_PI) as libc::c_short;
        if unsafe { libc::ioctl(tap.as_raw_fd(), libc::TUNSETIFF, &mut req) } < 0 {
            let error = io::Error::last_os_error();
            match error.raw_os_error() {
                Some(libc::EBUSY) => {
                    bail!("tap {name} is busy (already attached to another process)")
                }
                Some(libc::EINVAL) => bail!("{name} is not a single-queue TAP device"),
                _ => return Err(error).with_context(|| format!("attach tap {name}")),
            }
        }
        let wake = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
        if wake < 0 {
            return Err(io::Error::last_os_error()).context("create virtio-net eventfd");
        }
        // eventfd returned a new, uniquely owned fd.
        let wake = unsafe { File::from_raw_fd(wake) };
        Ok(Self {
            tap,
            wake,
            stats: Stats::default(),
            logged_errors: 0,
            tap_broken: false,
            rx_frame: vec![0; HEADER + MAX_FRAME],
            rx_len: 0,
        })
    }

    pub fn print_info(name: &str) {
        eprintln!(
            "INFO: virtio-net {name}: mac 52:54:00:12:34:56, mmio {BASE:#x} irq {}",
            Self::IRQ
        );
    }

    pub fn print_stats(&self, transport: &TransportStats) {
        let s = &self.stats;
        eprintln!(
            "virtio-net: tx_packets={} tx_bytes={} tx_errors={} rx_packets={} rx_bytes={} rx_dropped={} bad_chains={} notifies={} interrupts={}",
            s.tx_packets,
            s.tx_bytes,
            s.tx_errors,
            s.rx_packets,
            s.rx_bytes,
            s.rx_dropped,
            s.bad_chains,
            transport.notifies,
            transport.interrupts
        );
    }

    fn log(&mut self, error: impl std::fmt::Display) {
        if self.logged_errors < 15 {
            self.logged_errors += 1;
            eprintln!("virtio-net: {error}");
        } else if self.logged_errors == 15 {
            self.logged_errors += 1;
            eprintln!("virtio-net: further errors are not logged");
        }
    }

    pub fn fds(&self) -> (RawFd, RawFd) {
        (self.wake.as_raw_fd(), self.tap.as_raw_fd())
    }

    pub fn wake(&self) -> Result<()> {
        loop {
            match (&self.wake).write_all(&1u64.to_ne_bytes()) {
                Ok(()) => return Ok(()),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(()),
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => return Err(error).context("wake virtio-net RX thread"),
            }
        }
    }

    pub fn drain_wake(&self) -> Result<()> {
        let mut bytes = [0; 8];
        loop {
            match (&self.wake).read(&mut bytes) {
                Ok(_) => return Ok(()),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(()),
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => return Err(error).context("read virtio-net eventfd"),
            }
        }
    }

    fn tx(&mut self, queue: &Queue, dma: &Dma<'_>, head: u16) {
        let frame = (|| -> Result<Vec<u8>> {
            let (chain, total) = network_chain(queue, dma, head, false)?;
            ensure!(
                (HEADER + ETH_HEADER..=HEADER + MAX_FRAME).contains(&total),
                "TX chain has invalid length {total}"
            );
            let mut frame = vec![0; total];
            let mut offset = 0;
            for desc in chain {
                let end = offset + desc.len;
                dma.read(desc.addr, &mut frame[offset..end])?;
                offset = end;
            }
            Ok(frame)
        })();
        let frame = match frame {
            Ok(frame) => frame,
            Err(error) => {
                self.stats.bad_chains += 1;
                self.log(error);
                return;
            }
        };

        // A single TAP write is one Ethernet frame. Retrying a short write
        // would send the remainder as an independent frame.
        let frame = &frame[HEADER..];
        loop {
            match (&self.tap).write(frame) {
                Ok(count) if count == frame.len() => {
                    self.stats.tx_packets += 1;
                    self.stats.tx_bytes += count as u64;
                    return;
                }
                Ok(_) => self.log("TX write to tap failed: short write"),
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => self.log(format_args!("TX write to tap failed: {error}")),
            }
            self.stats.tx_errors += 1;
            return;
        }
    }

    fn have_frame(&mut self) -> bool {
        if self.rx_len != 0 {
            return true;
        }
        if self.tap_broken {
            return false;
        }
        loop {
            match (&self.tap).read(&mut self.rx_frame[HEADER..]) {
                Ok(count) if count != 0 => {
                    self.rx_len = count;
                    return true;
                }
                Ok(_) => self.log("read from tap failed: end of file; receive path disabled"),
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return false,
                Err(error) => self.log(format_args!(
                    "read from tap failed: {error}; receive path disabled"
                )),
            }
            self.tap_broken = true;
            return false;
        }
    }

    /// Returns whether the worker should poll TAP along with its wake event.
    pub fn receive(&mut self, queue: &mut Queue, dma: &Dma<'_>) -> Result<bool> {
        for _ in 0..queue.num {
            // Leave frames in the TAP receive queue while the guest has no buffers.
            let Some(head) = queue.peek(dma)? else {
                return Ok(false);
            };
            if !self.have_frame() {
                return Ok(!self.tap_broken);
            }
            let (chain, capacity) = match network_chain(queue, dma, head, true) {
                Ok(chain) => chain,
                Err(error) => {
                    self.stats.bad_chains += 1;
                    self.log(error);
                    queue.complete(dma, head, 0)?;
                    continue;
                }
            };

            let size = HEADER + self.rx_len;
            if capacity < size {
                self.stats.rx_dropped += 1;
                let frame_len = self.rx_len;
                let buffer_len = capacity.saturating_sub(HEADER);
                self.log(format_args!(
                    "RX frame of {frame_len} bytes dropped: guest buffer holds {buffer_len}"
                ));
                self.rx_len = 0;
                continue;
            }

            self.rx_frame[10] = 1; // num_buffers in the modern virtio-net header.
            let mut remaining = &self.rx_frame[..size];
            for desc in chain {
                let count = remaining.len().min(desc.len);
                dma.write(desc.addr, &remaining[..count])?;
                remaining = &remaining[count..];
                if remaining.is_empty() {
                    break;
                }
            }
            queue.complete(dma, head, size as u32)?;
            self.stats.rx_packets += 1;
            self.stats.rx_bytes += self.rx_len as u64;
            self.rx_len = 0;
        }
        let available = queue.peek(dma)?.is_some();
        if available && self.rx_len != 0 {
            self.wake()?;
        }
        Ok(available && !self.tap_broken)
    }
}

impl Backend for Net {
    const DEVICE_ID: u32 = 1;
    const IRQ: u32 = 6;
    const FEATURES: u64 = 1 << 5; // MAC
    const QUEUES: usize = 2;
    const QUEUE_SIZE: u32 = 256;

    fn config(&self) -> &[u8] {
        &MAC
    }

    fn notify(&mut self, index: usize, queue: &mut Queue, dma: &Dma<'_>) -> Result<()> {
        if index == RX {
            return self.wake();
        }
        for _ in 0..queue.num {
            let Some(head) = queue.peek(dma)? else {
                break;
            };
            self.tx(queue, dma, head);
            queue.complete(dma, head, 0)?;
        }
        Ok(())
    }

    fn driver_ok(&self) -> Result<()> {
        self.wake()
    }
}

fn network_chain(
    queue: &Queue,
    dma: &Dma<'_>,
    head: u16,
    writable: bool,
) -> Result<(Vec<Desc>, usize)> {
    let chain = queue.chain(dma, head)?;
    let mut total = 0;
    for desc in &chain {
        ensure!(desc.writable == writable, "invalid net buffer direction");
        dma.check(desc.addr, desc.len)?;
        total += desc.len;
    }
    Ok((chain, total))
}

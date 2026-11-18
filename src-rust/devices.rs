mod blk;
mod ioapic;
mod net;
mod queue;
mod transport;
mod uart;

use std::fs::File;
use std::io;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

use anyhow::{Context, Result, ensure};
use kvm_ioctls::VmFd;

use crate::memory::{Dma, Memory};

use self::blk::Block;
use self::ioapic::Ioapic;
use self::net::Net;
use self::queue::{QueueMemory, read_u16, read_u32, read_u64};
use self::transport::{MMIO_SIZE, Transport};
use self::uart::{COM1, Uart};

pub const SERIAL_STATE_TAG: u32 = 30;
pub const BLOCK_STATE_TAG: u32 = 31;
pub const NET_STATE_TAG: u32 = 32;

#[derive(Debug)]
pub struct OpenError {
    pub stage: &'static str,
    cause: anyhow::Error,
}

impl std::fmt::Display for OpenError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{}", self.cause)
    }
}

impl std::error::Error for OpenError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        self.cause.source()
    }
}

struct Interrupts {
    vm: Arc<VmFd>,
    ioapic: Option<Mutex<Ioapic>>,
}

impl Interrupts {
    fn set(&self, pin: u32, level: bool) -> Result<()> {
        if let Some(ioapic) = &self.ioapic {
            ioapic.lock().unwrap().signal(pin, level, &self.vm)
        } else {
            self.vm
                .set_irq_line(pin, level)
                .with_context(|| format!("KVM_IRQ_LINE {pin}"))
        }
    }
}

pub struct Devices {
    memory: Arc<Memory>,
    interrupts: Interrupts,
    uart: Uart,
    block: Option<Mutex<Transport<Block>>>,
    net: Option<Mutex<Transport<Net>>>,
    rx_stop: AtomicBool,
    rx_running: AtomicBool,
}

impl Devices {
    pub fn new(
        memory: Arc<Memory>,
        vm: Arc<VmFd>,
        disk: Option<&Path>,
        tap: Option<&str>,
        tdx: bool,
    ) -> Result<Self> {
        let block = disk
            .map(|path| {
                let block = Block::open(path).map_err(|cause| OpenError {
                    stage: "failed_disk_open",
                    cause,
                })?;
                block.print_info(path);
                Ok::<_, OpenError>(Mutex::new(Transport::new(block, tdx)))
            })
            .transpose()?;
        let net = tap
            .map(|name| {
                let net = Net::open(name).map_err(|cause| OpenError {
                    stage: "failed_net_open",
                    cause,
                })?;
                Net::print_info(name);
                Ok::<_, OpenError>(Mutex::new(Transport::new(net, tdx)))
            })
            .transpose()?;
        Ok(Self {
            memory,
            interrupts: Interrupts {
                vm,
                ioapic: tdx.then(|| Mutex::new(Ioapic::new())),
            },
            uart: Uart::default(),
            block,
            net,
            rx_stop: AtomicBool::new(false),
            rx_running: AtomicBool::new(false),
        })
    }

    pub fn io_read(&self, port: u16, width: usize, data: &mut [u8]) -> Result<bool> {
        if !(COM1..COM1 + 8).contains(&port) {
            return Ok(false);
        }
        self.uart.read(port - COM1, width, data, &self.interrupts)?;
        Ok(true)
    }

    pub fn io_write(&self, port: u16, data: &[u8]) -> Result<bool> {
        if !(COM1..COM1 + 8).contains(&port) {
            return Ok(false);
        }
        self.uart.write(port - COM1, data, &self.interrupts)?;
        Ok(true)
    }

    pub fn mmio(&self, mut gpa: u64, data: &mut [u8], write: bool) -> Result<bool> {
        if self.interrupts.ioapic.is_some() {
            gpa &= !(1_u64 << 47); // TD GPAW=48
        }
        if let Some(block) = &self.block {
            if (blk::BASE..blk::BASE + MMIO_SIZE).contains(&gpa) {
                block.lock().unwrap().access(
                    gpa - blk::BASE,
                    data,
                    write,
                    &self.memory,
                    &self.interrupts,
                )?;
                return Ok(true);
            }
        }
        if let Some(net) = &self.net {
            if (net::BASE..net::BASE + MMIO_SIZE).contains(&gpa) {
                net.lock().unwrap().access(
                    gpa - net::BASE,
                    data,
                    write,
                    &self.memory,
                    &self.interrupts,
                )?;
                return Ok(true);
            }
        }
        if let Some(ioapic) = &self.interrupts.ioapic {
            if (ioapic::BASE..ioapic::BASE + MMIO_SIZE).contains(&gpa) {
                ioapic.lock().unwrap().access(
                    gpa - ioapic::BASE,
                    data,
                    write,
                    &self.interrupts.vm,
                )?;
                return Ok(true);
            }
        }
        Ok(false)
    }

    pub fn serial_input(&self, data: &[u8]) -> Result<()> {
        self.uart.input(data, &self.interrupts)
    }

    pub fn close_input(&self) {
        self.uart.close_input();
    }

    pub fn start_rx(self: &Arc<Self>) -> Result<Option<JoinHandle<Result<()>>>> {
        let Some(net) = &self.net else {
            return Ok(None);
        };
        ensure!(
            !self.rx_running.swap(true, Ordering::AcqRel),
            "virtio-net RX is already running"
        );
        let (wake, tap) = {
            let net = net.lock().unwrap();
            self.rx_stop.store(false, Ordering::Release);
            net.backend.fds()
        };
        let devices = Arc::clone(self);
        let worker = thread::Builder::new()
            .name("svmm-net-rx".to_owned())
            .spawn(move || {
                let result = devices.rx_worker(wake, tap);
                devices.rx_running.store(false, Ordering::Release);
                result
            });
        match worker {
            Ok(worker) => Ok(Some(worker)),
            Err(error) => {
                self.rx_running.store(false, Ordering::Release);
                Err(error).context("start virtio-net RX thread")
            }
        }
    }

    pub fn stop_rx(&self) -> Result<()> {
        if let Some(net) = &self.net {
            let net = net.lock().unwrap();
            self.rx_stop.store(true, Ordering::Release);
            net.backend.wake()?;
        }
        Ok(())
    }

    fn rx_worker(&self, wake: i32, tap: i32) -> Result<()> {
        let net = self.net.as_ref().expect("RX worker has a TAP");
        while !self.rx_stop.load(Ordering::Acquire) {
            let want_tap = net
                .lock()
                .unwrap()
                .run_queue(0, &self.memory, &self.interrupts, false, Net::receive)?
                .unwrap_or(false);
            let mut polls = [
                libc::pollfd {
                    fd: wake,
                    events: libc::POLLIN,
                    revents: 0,
                },
                libc::pollfd {
                    fd: tap,
                    events: libc::POLLIN,
                    revents: 0,
                },
            ];
            // Both fd owners remain alive through this worker's Arc<Devices>.
            let result =
                unsafe { libc::poll(polls.as_mut_ptr(), if want_tap { 2 } else { 1 }, -1) };
            if result < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(error).context("poll virtio-net TAP");
            }
            ensure!(
                polls[0].revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) == 0,
                "virtio-net wake eventfd is broken"
            );
            if polls[0].revents & libc::POLLIN != 0 {
                net.lock().unwrap().backend.drain_wake()?;
            }
        }
        Ok(())
    }

    pub fn print_stats(&self) {
        if let Some(block) = &self.block {
            let block = block.lock().unwrap();
            block.backend.print_stats(&block.stats);
        }
        if let Some(net) = &self.net {
            let net = net.lock().unwrap();
            net.backend.print_stats(&net.stats);
        }
    }

    pub fn disk_file(&self) -> Result<Option<File>> {
        Ok(self
            .block
            .as_ref()
            .map(|block| block.lock().unwrap().backend.file.try_clone())
            .transpose()?)
    }

    /// Capture device sections and the kernel interrupt state at the same point.
    pub fn snapshot_with<T>(
        &self,
        capture: impl FnOnce(&[(u32, Vec<u8>)]) -> Result<T>,
    ) -> Result<T> {
        let uart = self.uart.state.lock().unwrap();
        let block = self.block.as_ref().map(|dev| dev.lock().unwrap());
        let net = self.net.as_ref().map(|dev| dev.lock().unwrap());
        let mut sections = vec![(SERIAL_STATE_TAG, uart.encode())];
        if let Some(block) = &block {
            sections.push((BLOCK_STATE_TAG, block.encode()));
        }
        if let Some(net) = &net {
            sections.push((NET_STATE_TAG, net.encode()));
        }
        capture(&sections)
    }

    pub fn restore_sections(&self, sections: &[(u32, Vec<u8>)]) -> Result<()> {
        let mut uart = self.uart.state.lock().unwrap();
        let mut block = self.block.as_ref().map(|dev| dev.lock().unwrap());
        let mut net = self.net.as_ref().map(|dev| dev.lock().unwrap());
        let dma = self.memory.dma();
        let mut uart_state = None;
        let mut block_state = None;
        let mut net_state = None;
        for (tag, payload) in sections {
            match *tag {
                SERIAL_STATE_TAG => {
                    ensure!(uart_state.is_none(), "duplicate saved serial state");
                    uart_state = Some(uart::State::decode(payload)?);
                }
                BLOCK_STATE_TAG => {
                    ensure!(block_state.is_none(), "duplicate saved virtio-blk state");
                    let device = block
                        .as_ref()
                        .context("saved virtio-blk device requires a disk")?;
                    block_state = Some(device.decode(payload, &dma).context("restore virtio-blk")?);
                }
                NET_STATE_TAG => {
                    ensure!(net_state.is_none(), "duplicate saved virtio-net state");
                    let device = net
                        .as_ref()
                        .context("saved virtio-net device requires a TAP")?;
                    net_state = Some(device.decode(payload, &dma).context("restore virtio-net")?);
                }
                _ => anyhow::bail!("unknown device state section {tag}"),
            }
        }

        let uart_state = uart_state.context("missing saved serial state")?;
        ensure!(
            block.is_some() == block_state.is_some(),
            "missing saved virtio-blk state"
        );
        ensure!(
            net.is_some() == net_state.is_some(),
            "missing saved virtio-net state"
        );

        // KVM's IRQ controller already carries the saved line levels.
        *uart = uart_state;
        if let Some((device, state)) = block.as_mut().zip(block_state) {
            device.state = state;
        }
        if let Some((device, state)) = net.as_mut().zip(net_state) {
            device.state = state;
        }
        Ok(())
    }
}

impl QueueMemory for Dma<'_> {
    fn check(&self, addr: u64, len: usize) -> Result<()> {
        Dma::check(self, addr, len)
    }

    fn read(&self, addr: u64, dst: &mut [u8]) -> Result<()> {
        Dma::read(self, addr, dst)
    }

    fn write(&self, addr: u64, src: &[u8]) -> Result<()> {
        Dma::write(self, addr, src)
    }

    fn read_u16_acquire(&self, addr: u64) -> Result<u16> {
        Dma::read_u16_acquire(self, addr)
    }

    fn write_u16_release(&self, addr: u64, value: u16) -> Result<()> {
        Dma::write_u16_release(self, addr, value)
    }
}

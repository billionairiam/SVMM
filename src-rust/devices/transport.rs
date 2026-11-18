use std::sync::atomic::{Ordering, fence};

use anyhow::{Context, Result, ensure};

use crate::memory::{Dma, Memory};

use super::queue::Queue;
use super::{Interrupts, read_u16, read_u32, read_u64};

const VERSION_1: u64 = 1 << 32;
const ACCESS_PLATFORM: u64 = 1 << 33;
const DRIVER_OK: u32 = 4;
const FEATURES_OK: u32 = 8;
const NEEDS_RESET: u32 = 0x40;
const FAILED: u32 = 0x80;
const STATUS_DRIVER_BITS: u32 = 3 | DRIVER_OK | FEATURES_OK | FAILED;
const VRING_INTERRUPT: u32 = 1;
const CONFIG_INTERRUPT: u32 = 2;
const MAX_QUEUES: usize = 2;

pub const MMIO_SIZE: u64 = 0x1000;
pub const STATE_SIZE: usize = 192;

pub trait Backend {
    const DEVICE_ID: u32;
    const IRQ: u32;
    const FEATURES: u64;
    const QUEUES: usize;
    const QUEUE_SIZE: u32;

    fn config(&self) -> &[u8];
    fn notify(&mut self, index: usize, queue: &mut Queue, dma: &Dma<'_>) -> Result<()>;
    fn driver_ok(&self) -> Result<()> {
        Ok(())
    }
}

#[derive(Default)]
pub struct State {
    status: u32,
    device_features_sel: u32,
    driver_features_sel: u32,
    driver_features: u64,
    queue_sel: u32,
    interrupt_status: u32,
    irq_level: bool,
    queues: [Queue; MAX_QUEUES],
}

#[derive(Default)]
pub struct Stats {
    pub notifies: u64,
    pub interrupts: u64,
}

pub struct Transport<B> {
    pub backend: B,
    pub state: State,
    pub stats: Stats,
    required_features: u64,
}

impl<B: Backend> Transport<B> {
    pub fn new(backend: B, tdx: bool) -> Self {
        Self {
            backend,
            state: State::default(),
            stats: Stats::default(),
            required_features: VERSION_1 | if tdx { ACCESS_PLATFORM } else { 0 },
        }
    }

    fn features(&self) -> u64 {
        self.required_features | B::FEATURES
    }

    fn update_irq(&mut self, interrupts: &Interrupts) -> Result<()> {
        let level = self.state.interrupt_status != 0;
        if level != self.state.irq_level {
            interrupts.set(B::IRQ, level)?;
            self.state.irq_level = level;
            self.stats.interrupts += u64::from(level);
        }
        Ok(())
    }

    fn selected_queue(&mut self) -> Option<&mut Queue> {
        self.state.queues[..B::QUEUES].get_mut(self.state.queue_sel as usize)
    }

    fn set_status(&mut self, value: u32, interrupts: &Interrupts) -> Result<()> {
        let old = self.state.status;
        if value == 0 {
            let irq_level = self.state.irq_level;
            self.state = State {
                irq_level,
                ..State::default()
            };
            return self.update_irq(interrupts);
        }
        let mut next = (value & STATUS_DRIVER_BITS) | (old & NEEDS_RESET);
        if self.state.driver_features & !self.features() != 0
            || self.state.driver_features & self.required_features != self.required_features
        {
            next &= !FEATURES_OK;
        }
        next |= old & FEATURES_OK;
        self.state.status = next;
        if next & DRIVER_OK != 0 && old & DRIVER_OK == 0 {
            self.backend.driver_ok()?;
        }
        Ok(())
    }

    fn read_register(&self, offset: u64) -> u32 {
        match offset {
            0x000 => 0x7472_6976, // "virt"
            0x004 => 2,
            0x008 => B::DEVICE_ID,
            0x00c => 0x4d4d_5653, // "SVMM"
            0x010 => feature_window(self.features(), self.state.device_features_sel),
            0x034 if (self.state.queue_sel as usize) < B::QUEUES => B::QUEUE_SIZE,
            0x044 => self.state.queues[..B::QUEUES]
                .get(self.state.queue_sel as usize)
                .map_or(0, |queue| u32::from(queue.ready)),
            0x060 => self.state.interrupt_status,
            0x070 => self.state.status,
            0x0b0 | 0x0b4 | 0x0b8 | 0x0bc => u32::MAX, // No shared memory region.
            _ => 0,
        }
    }

    pub fn access(
        &mut self,
        offset: u64,
        bytes: &mut [u8],
        write: bool,
        memory: &Memory,
        interrupts: &Interrupts,
    ) -> Result<()> {
        if !matches!(bytes.len(), 1 | 2 | 4 | 8) || offset + bytes.len() as u64 > MMIO_SIZE {
            if !write {
                bytes.fill(0);
            }
        } else if offset >= 0x100 {
            if !write {
                bytes.fill(0);
                if let Some(config) = self.backend.config().get((offset - 0x100) as usize..) {
                    let count = config.len().min(bytes.len());
                    bytes[..count].copy_from_slice(&config[..count]);
                }
            }
        } else if bytes.len() != 4 || offset & 3 != 0 {
            if !write {
                bytes.fill(0);
            }
        } else if write {
            self.write_register(offset, read_u32(bytes, 0), memory, interrupts)?;
        } else {
            bytes.copy_from_slice(&self.read_register(offset).to_le_bytes());
        }
        Ok(())
    }

    fn write_register(
        &mut self,
        offset: u64,
        value: u32,
        memory: &Memory,
        interrupts: &Interrupts,
    ) -> Result<()> {
        match offset {
            0x014 => self.state.device_features_sel = value,
            0x020
                if self.state.status & FEATURES_OK == 0 && self.state.driver_features_sel <= 1 =>
            {
                write_half(
                    &mut self.state.driver_features,
                    value,
                    self.state.driver_features_sel == 1,
                );
            }
            0x024 => self.state.driver_features_sel = value,
            0x030 => self.state.queue_sel = value,
            0x038 => {
                if let Some(queue) = self.selected_queue().filter(|queue| !queue.ready) {
                    queue.num = value;
                }
            }
            0x044 => {
                let features_ok = self.state.status & FEATURES_OK != 0;
                if let Some(queue) = self.selected_queue() {
                    if value == 0 {
                        queue.ready = false;
                    } else if value == 1
                        && !queue.ready
                        && features_ok
                        && queue.validate(&memory.dma(), B::QUEUE_SIZE).is_ok()
                    {
                        queue.ready = true;
                        queue.last_seen_avail = 0;
                        queue.next_used_idx = 0;
                    }
                }
            }
            0x050 => {
                self.run_queue(
                    value as usize,
                    memory,
                    interrupts,
                    true,
                    |backend, queue, dma| backend.notify(value as usize, queue, dma),
                )?;
            }
            0x064 => {
                self.state.interrupt_status &= !value;
                self.update_irq(interrupts)?;
            }
            0x070 => self.set_status(value, interrupts)?,
            0x080 | 0x084 | 0x090 | 0x094 | 0x0a0 | 0x0a4 => {
                if let Some(queue) = self.selected_queue().filter(|queue| !queue.ready) {
                    let addr = match offset & !4 {
                        0x080 => &mut queue.desc,
                        0x090 => &mut queue.avail,
                        _ => &mut queue.used,
                    };
                    write_half(addr, value, offset & 4 != 0);
                }
            }
            _ => {}
        }
        Ok(())
    }

    pub fn run_queue<T>(
        &mut self,
        index: usize,
        memory: &Memory,
        interrupts: &Interrupts,
        notify: bool,
        process: impl FnOnce(&mut B, &mut Queue, &Dma<'_>) -> Result<T>,
    ) -> Result<Option<T>> {
        if self.state.status & (FEATURES_OK | DRIVER_OK | NEEDS_RESET | FAILED)
            != FEATURES_OK | DRIVER_OK
        {
            return Ok(None);
        }
        let Some(queue) = self.state.queues[..B::QUEUES]
            .get_mut(index)
            .filter(|q| q.ready)
        else {
            return Ok(None);
        };

        // Retain the DMA guard during validation and use. TDX may convert ring pages
        // back to private between notifications.
        let dma = memory.dma();
        let before = queue.next_used_idx;
        let result = queue.validate(&dma, B::QUEUE_SIZE).and_then(|()| {
            self.stats.notifies += u64::from(notify);
            process(&mut self.backend, queue, &dma)
        });
        if queue.next_used_idx != before {
            // Preserve used.idx publication before checking the driver's suppression flag.
            fence(Ordering::SeqCst);
            if queue.needs_interrupt(&dma).unwrap_or(true) {
                self.state.interrupt_status |= VRING_INTERRUPT;
            }
        }
        drop(dma);
        if result.is_err() {
            self.state.status |= NEEDS_RESET;
            self.state.interrupt_status |= CONFIG_INTERRUPT;
        }
        self.update_irq(interrupts)?;
        Ok(result.ok())
    }

    pub fn encode(&self) -> Vec<u8> {
        // Stage 11 virtio_mmio_state, including its padding and unused queue slots.
        let s = &self.state;
        let mut bytes = vec![0; STATE_SIZE];
        put32(&mut bytes, 0, B::DEVICE_ID);
        put32(&mut bytes, 4, B::QUEUES as u32);
        put32(&mut bytes, 8, s.status);
        put32(&mut bytes, 12, s.device_features_sel);
        put32(&mut bytes, 16, s.driver_features_sel);
        put32(&mut bytes, 20, s.queue_sel);
        put64(&mut bytes, 24, s.driver_features);
        put32(&mut bytes, 32, s.interrupt_status);
        put32(&mut bytes, 36, u32::from(s.irq_level));
        let config = self.backend.config();
        put32(&mut bytes, 40, config.len() as u32);
        bytes[48..48 + config.len()].copy_from_slice(config);
        for (index, queue) in s.queues[..B::QUEUES].iter().enumerate() {
            let pos = 112 + index * 40;
            put64(&mut bytes, pos, queue.desc);
            put64(&mut bytes, pos + 8, queue.avail);
            put64(&mut bytes, pos + 16, queue.used);
            put32(&mut bytes, pos + 24, queue.num);
            put32(&mut bytes, pos + 28, u32::from(queue.ready));
            if queue.ready {
                bytes[pos + 32..pos + 34].copy_from_slice(&queue.last_seen_avail.to_le_bytes());
                bytes[pos + 34..pos + 36].copy_from_slice(&queue.next_used_idx.to_le_bytes());
            }
        }
        bytes
    }

    pub fn decode(&self, bytes: &[u8], dma: &Dma<'_>) -> Result<State> {
        ensure!(bytes.len() == STATE_SIZE, "invalid virtio snapshot size");
        ensure!(
            read_u32(bytes, 0) == B::DEVICE_ID && read_u32(bytes, 4) == B::QUEUES as u32,
            "virtio device type or queue count differs"
        );
        let config = self.backend.config();
        ensure!(
            read_u32(bytes, 40) as usize == config.len() && bytes[48..48 + config.len()] == *config,
            "virtio configuration space differs (disk size / MAC)"
        );
        let status = read_u32(bytes, 8);
        let driver_features = read_u64(bytes, 24);
        ensure!(
            driver_features & !self.features() == 0
                && (status & FEATURES_OK == 0
                    || driver_features & self.required_features == self.required_features),
            "saved virtio driver features are invalid"
        );
        let queue_sel = read_u32(bytes, 20);
        let interrupt_status = read_u32(bytes, 32);
        let irq_level = read_u32(bytes, 36);
        ensure!(
            queue_sel < MAX_QUEUES as u32
                && irq_level == u32::from(interrupt_status != 0)
                && interrupt_status & !(VRING_INTERRUPT | CONFIG_INTERRUPT) == 0
                && status & !(STATUS_DRIVER_BITS | NEEDS_RESET) == 0,
            "saved virtio register state is inconsistent"
        );
        let mut queues = [Queue::default(); MAX_QUEUES];
        for (index, queue) in queues[..B::QUEUES].iter_mut().enumerate() {
            let pos = 112 + index * 40;
            let ready = read_u32(bytes, pos + 28);
            ensure!(ready <= 1, "saved virtio queue readiness is invalid");
            *queue = Queue {
                desc: read_u64(bytes, pos),
                avail: read_u64(bytes, pos + 8),
                used: read_u64(bytes, pos + 16),
                num: read_u32(bytes, pos + 24),
                ready: ready != 0,
                last_seen_avail: read_u16(bytes, pos + 32),
                next_used_idx: read_u16(bytes, pos + 34),
            };
            if queue.ready {
                queue
                    .validate(dma, B::QUEUE_SIZE)
                    .with_context(|| format!("invalid saved virtio queue {index}"))?;
            }
        }
        Ok(State {
            status,
            device_features_sel: read_u32(bytes, 12),
            driver_features_sel: read_u32(bytes, 16),
            driver_features,
            queue_sel,
            interrupt_status,
            irq_level: irq_level != 0,
            queues,
        })
    }
}

fn feature_window(features: u64, select: u32) -> u32 {
    features
        .checked_shr(32_u32.saturating_mul(select))
        .unwrap_or(0) as u32
}

fn write_half(dest: &mut u64, value: u32, high: bool) {
    let shift = u32::from(high) * 32;
    let mask = u64::from(u32::MAX) << shift;
    *dest = (*dest & !mask) | u64::from(value) << shift;
}

fn put32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

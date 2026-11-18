use std::collections::VecDeque;
use std::io::{self, Write};
use std::sync::{Condvar, Mutex};

use anyhow::{Result, ensure};

use super::Interrupts;

pub const COM1: u16 = 0x3f8;
const IRQ: u32 = 4;
const RX_CAPACITY: usize = 4096;
const FIFO_SIZE: usize = 16;
const DLAB: u8 = 0x80;
const OUT2: u8 = 0x08;
const LOOPBACK: u8 = 0x10;
const RX_INTERRUPT: u8 = 4;
const THR_INTERRUPT: u8 = 2;
const NO_INTERRUPT: u8 = 1;
pub const STATE_SIZE: usize = 16 + RX_CAPACITY;

#[derive(Default)]
pub struct Uart {
    pub state: Mutex<State>,
    rx_space: Condvar,
}

#[derive(Default)]
pub struct State {
    ier: u8,
    lcr: u8,
    mcr: u8,
    fcr: u8,
    scr: u8,
    dll: u8,
    dlm: u8,
    thre_pending: bool,
    irq_level: bool,
    rx_closed: bool,
    rx: VecDeque<u8>,
}

impl State {
    fn iir(&self) -> u8 {
        let fifo = if self.fcr & 1 != 0 { 0xc0 } else { 0 };
        fifo | if self.ier & 1 != 0 && !self.rx.is_empty() {
            RX_INTERRUPT
        } else if self.ier & 2 != 0 && self.thre_pending {
            THR_INTERRUPT
        } else {
            NO_INTERRUPT
        }
    }

    fn update_irq(&mut self, interrupts: &Interrupts) -> Result<()> {
        let level = self.iir() & NO_INTERRUPT == 0 && self.mcr & OUT2 != 0;
        if self.irq_level != level {
            interrupts.set(IRQ, level)?;
            self.irq_level = level;
        }
        Ok(())
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut bytes = vec![0; STATE_SIZE];
        bytes[..9].copy_from_slice(&[
            self.ier,
            self.lcr,
            self.mcr,
            self.fcr,
            self.scr,
            self.dll,
            self.dlm,
            u8::from(self.thre_pending),
            u8::from(self.irq_level),
        ]);
        bytes[12..16].copy_from_slice(&(self.rx.len() as u32).to_le_bytes());
        for (slot, byte) in bytes[16..].iter_mut().zip(&self.rx) {
            *slot = *byte;
        }
        bytes
    }

    pub fn decode(bytes: &[u8]) -> Result<Self> {
        ensure!(bytes.len() == STATE_SIZE, "invalid serial snapshot size");
        let count = super::read_u32(bytes, 12) as usize;
        ensure!(
            count <= RX_CAPACITY && bytes[7] <= 1 && bytes[8] <= 1,
            "invalid saved serial state"
        );
        Ok(Self {
            ier: bytes[0],
            lcr: bytes[1],
            mcr: bytes[2],
            fcr: bytes[3],
            scr: bytes[4],
            dll: bytes[5],
            dlm: bytes[6],
            thre_pending: bytes[7] != 0,
            irq_level: bytes[8] != 0,
            rx: bytes[16..16 + count].iter().copied().collect(),
            rx_closed: false,
        })
    }
}

impl Uart {
    pub fn read(
        &self,
        register: u16,
        width: usize,
        data: &mut [u8],
        interrupts: &Interrupts,
    ) -> Result<()> {
        let mut state = self.state.lock().unwrap();
        data.fill(0);
        for byte in data.iter_mut().step_by(width) {
            *byte = match register {
                0 if state.lcr & DLAB != 0 => state.dll,
                0 => {
                    let byte = state.rx.pop_front();
                    if byte.is_some() {
                        self.rx_space.notify_all();
                    }
                    byte.unwrap_or(0)
                }
                1 if state.lcr & DLAB != 0 => state.dlm,
                1 => state.ier,
                2 => {
                    let value = state.iir();
                    if value & 0xf == THR_INTERRUPT {
                        state.thre_pending = false;
                    }
                    value
                }
                3 => state.lcr,
                4 => state.mcr,
                5 => 0x60 | u8::from(!state.rx.is_empty()),
                6 if state.mcr & LOOPBACK != 0 => {
                    (state.mcr & 2) << 3 | (state.mcr & 1) << 5 | (state.mcr & 0x0c) << 4
                }
                6 => 0xb0,
                7 => state.scr,
                _ => unreachable!("validated COM1 register"),
            };
            state.update_irq(interrupts)?;
        }
        Ok(())
    }

    pub fn write(&self, register: u16, data: &[u8], interrupts: &Interrupts) -> Result<()> {
        let Some(&byte) = data.first() else {
            return Ok(());
        };
        let mut state = self.state.lock().unwrap();
        let mut transmit = false;
        match register {
            0 if state.lcr & DLAB != 0 => state.dll = byte,
            0 => {
                if state.mcr & LOOPBACK != 0 {
                    let limit = FIFO_SIZE.saturating_sub(state.rx.len());
                    state.rx.extend(data.iter().take(limit).copied());
                } else {
                    transmit = true;
                }
                state.thre_pending = true;
            }
            1 if state.lcr & DLAB != 0 => state.dlm = byte,
            1 => {
                if byte & 2 != 0 && state.ier & 2 == 0 {
                    state.thre_pending = true;
                }
                state.ier = byte & 0xf;
            }
            2 => {
                state.fcr = byte & 1;
                if byte & 2 != 0 {
                    state.rx.clear();
                    self.rx_space.notify_all();
                }
            }
            3 => state.lcr = byte,
            4 => state.mcr = byte,
            7 => state.scr = byte,
            _ => {}
        }
        state.update_irq(interrupts)?;
        drop(state);

        if transmit {
            let mut out = io::stdout().lock();
            out.write_all(data)?;
            out.flush()?;
        }
        Ok(())
    }

    pub fn input(&self, mut data: &[u8], interrupts: &Interrupts) -> Result<()> {
        let mut state = self.state.lock().unwrap();
        while !data.is_empty() {
            state = self
                .rx_space
                .wait_while(state, |s| s.rx.len() == RX_CAPACITY && !s.rx_closed)
                .unwrap();
            if state.rx_closed {
                return Err(io::Error::from(io::ErrorKind::BrokenPipe).into());
            }
            let count = data.len().min(RX_CAPACITY - state.rx.len());
            state.rx.extend(data[..count].iter().copied());
            state.update_irq(interrupts)?;
            data = &data[count..];
        }
        Ok(())
    }

    pub fn close_input(&self) {
        self.state.lock().unwrap().rx_closed = true;
        self.rx_space.notify_all();
    }
}

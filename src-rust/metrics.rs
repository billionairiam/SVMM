use kvm_bindings::*;
use kvm_ioctls::VcpuExit;
use std::{cmp::Reverse, collections::BTreeMap};

const PORT_LIMIT: usize = 32;

#[derive(Default)]
pub struct Stats {
    pub exits: u64,
    pub serial: u64,
    reasons: BTreeMap<u32, u64>,
    ports: BTreeMap<(u16, bool), u64>,
    other_ports: u64,
}

impl Stats {
    pub fn record(&mut self, exit: &VcpuExit<'_>) {
        self.exits += 1;
        let reason = match *exit {
            VcpuExit::IoIn(port, _) | VcpuExit::IoOut(port, _) => {
                self.add_port((port, matches!(exit, VcpuExit::IoOut(..))), 1);
                KVM_EXIT_IO
            }
            VcpuExit::MmioRead(..) | VcpuExit::MmioWrite(..) => KVM_EXIT_MMIO,
            VcpuExit::Unknown => KVM_EXIT_UNKNOWN,
            VcpuExit::Exception => KVM_EXIT_EXCEPTION,
            VcpuExit::Hypercall(_) => KVM_EXIT_HYPERCALL,
            VcpuExit::Debug(_) => KVM_EXIT_DEBUG,
            VcpuExit::Hlt => KVM_EXIT_HLT,
            VcpuExit::IrqWindowOpen => KVM_EXIT_IRQ_WINDOW_OPEN,
            VcpuExit::Shutdown => KVM_EXIT_SHUTDOWN,
            VcpuExit::FailEntry(..) => KVM_EXIT_FAIL_ENTRY,
            VcpuExit::Intr => KVM_EXIT_INTR,
            VcpuExit::InternalError => KVM_EXIT_INTERNAL_ERROR,
            VcpuExit::SystemEvent(..) => KVM_EXIT_SYSTEM_EVENT,
            VcpuExit::Unsupported(reason) => reason.min(63),
            _ => 63,
        };
        *self.reasons.entry(reason).or_default() += 1;
    }

    fn add_port(&mut self, key: (u16, bool), count: u64) {
        if let Some(current) = self.ports.get_mut(&key) {
            *current += count;
        } else if self.ports.len() < PORT_LIMIT {
            self.ports.insert(key, count);
        } else {
            self.other_ports += count;
        }
    }

    pub fn merge(&mut self, other: Self) {
        self.exits += other.exits;
        self.serial += other.serial;
        self.other_ports += other.other_ports;
        for (reason, count) in other.reasons {
            *self.reasons.entry(reason).or_default() += count;
        }
        for (key, count) in other.ports {
            self.add_port(key, count);
        }
    }

    pub fn print(&self) {
        if self.exits == 0 {
            return;
        }
        eprintln!(
            "VM exits handled by the VMM: {} (exits KVM handles in the kernel are not counted)",
            self.exits
        );
        eprintln!("  {:<26} {:>10} {:>8}", "exit reason", "exits", "share");
        for (&reason, &count) in &self.reasons {
            let name = match reason {
                KVM_EXIT_UNKNOWN => "KVM_EXIT_UNKNOWN",
                KVM_EXIT_EXCEPTION => "KVM_EXIT_EXCEPTION",
                KVM_EXIT_IO => "KVM_EXIT_IO",
                KVM_EXIT_HYPERCALL => "KVM_EXIT_HYPERCALL",
                KVM_EXIT_DEBUG => "KVM_EXIT_DEBUG",
                KVM_EXIT_HLT => "KVM_EXIT_HLT",
                KVM_EXIT_MMIO => "KVM_EXIT_MMIO",
                KVM_EXIT_IRQ_WINDOW_OPEN => "KVM_EXIT_IRQ_WINDOW_OPEN",
                KVM_EXIT_SHUTDOWN => "KVM_EXIT_SHUTDOWN",
                KVM_EXIT_FAIL_ENTRY => "KVM_EXIT_FAIL_ENTRY",
                KVM_EXIT_INTR => "KVM_EXIT_INTR",
                KVM_EXIT_INTERNAL_ERROR => "KVM_EXIT_INTERNAL_ERROR",
                KVM_EXIT_SYSTEM_EVENT => "KVM_EXIT_SYSTEM_EVENT",
                _ => "",
            };
            let fallback;
            let name = if name.is_empty() {
                fallback = format!(
                    "exit reason {reason}{}",
                    if reason == 63 { "+" } else { "" }
                );
                &fallback
            } else {
                name
            };
            let percent = 100.0 * count as f64 / self.exits as f64;
            eprintln!("  {name:<26} {count:>10} {percent:>7.2}%");
        }
        if self.ports.is_empty() {
            return;
        }
        let mut ports: Vec<_> = self.ports.iter().collect();
        ports.sort_unstable_by_key(|&(&key, &count)| (Reverse(count), key));
        eprintln!("  I/O exits by port (one REP INS/OUTS counts once):");
        eprintln!("  {:<8} {:<4} {:>10}  device", "port", "dir", "exits");
        for (&(port, out), &count) in ports {
            let direction = if out { "out" } else { "in" };
            eprintln!(
                "  0x{port:04x}   {direction:<4} {count:>10}  {}",
                port_name(port, out)
            );
        }
        if self.other_ports != 0 {
            eprintln!(
                "  {:<13} {:>10}  (port table full)",
                "other ports", self.other_ports
            );
        }
    }
}

fn port_name(port: u16, out: bool) -> &'static str {
    match port {
        0x3f8 if out => "COM1 THR/DLL (transmit byte)",
        0x3f8 => "COM1 RBR/DLL (receive byte)",
        0x3f9 => "COM1 IER/DLM",
        0x3fa if out => "COM1 FCR (FIFO control)",
        0x3fa => "COM1 IIR (interrupt id)",
        0x3fb => "COM1 LCR (line control)",
        0x3fc => "COM1 MCR (modem control)",
        0x3fd if out => "COM1 LSR (read-only)",
        0x3fd => "COM1 LSR (line status)",
        0x3fe if out => "COM1 MSR (read-only)",
        0x3fe => "COM1 MSR (modem status)",
        0x3ff => "COM1 SCR (scratch)",
        0x600 => "ACPI sleep control",
        0x601 => "ACPI sleep status",
        0xcf9 => "ACPI reset register",
        0xcf8 => "PCI config address (not emulated)",
        0xcfa..=0xcfb => "PCI config mechanism probe (not emulated)",
        0xcfc..=0xcff => "PCI config data (not emulated)",
        0x60 => "i8042 data (not emulated)",
        0x64 => "i8042 command/status (not emulated)",
        0x70 | 0x71 => "CMOS/RTC (not emulated)",
        0x80 => "POST/delay port (not emulated)",
        0x87 => "DMA page register (not emulated)",
        0x2f8..=0x2ff => "COM2 (not emulated)",
        0x3e8..=0x3ef => "COM3 (not emulated)",
        0x2e8..=0x2ef => "COM4 (not emulated)",
        _ => "(not emulated)",
    }
}

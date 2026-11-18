use std::io;
use std::os::fd::{AsRawFd, RawFd};
use std::sync::Arc;

use anyhow::{Context, Result, ensure};
use kvm_bindings::{
    CpuId, KVM_CAP_EXIT_HYPERCALL, KVM_CAP_GUEST_MEMFD, KVM_CAP_MAX_VCPUS,
    KVM_CAP_MEMORY_ATTRIBUTES, KVM_CAP_SIGNAL_MSI, KVM_CAP_SPLIT_IRQCHIP, KVM_CAP_VM_TYPES,
    KVM_CPUID_FLAG_SIGNIFCANT_INDEX, KVM_MEMORY_ATTRIBUTE_PRIVATE, KVM_X86_TDX_VM,
    kvm_cpuid_entry2, kvm_enable_cap,
};
use kvm_ioctls::{Kvm, VcpuFd, VmFd};

use super::td_shim::Staged;
use crate::memory::{Mapping, Memory, PAGE_SIZE};

// Linux x86 KVM UAPI. kvm-bindings currently lacks the TDX command payloads.
const KVM_MEMORY_ENCRYPT_OP: libc::Ioctl = libc::_IOWR::<libc::c_ulong>(0xae, 0xba);
const CAPABILITIES: u32 = 0;
const INIT_VM: u32 = 1;
const INIT_VCPU: u32 = 2;
const INIT_MEM_REGION: u32 = 3;
const FINALIZE_VM: u32 = 4;
const GET_CPUID: u32 = 5;
const MAP_GPA: u32 = 12;

const SEPT_VE_DISABLE: u64 = 1 << 28;
const XFAM_SSE: u64 = 3;
const XFAM_AVX: u64 = 1 << 2;
const CPUID_AVX: u32 = 1 << 28;
const CPUID_F16C: u32 = 1 << 29;
const CPUID_CAPACITY: usize = 256;
const KVM_SIGNATURE: u32 = 0x4000_0000;
const KVM_FEATURES: u32 = KVM_SIGNATURE + 1;

#[repr(C)]
struct Command {
    number: u32,
    flags: u32,
    data: u64,
    hardware_error: u64,
}

#[repr(C)]
struct Cpuid {
    count: u32,
    padding: u32,
    entries: [kvm_cpuid_entry2; CPUID_CAPACITY],
}

impl Cpuid {
    fn for_output() -> Self {
        Self::with_count(CPUID_CAPACITY as u32)
    }

    fn with_count(count: u32) -> Self {
        Self {
            count,
            padding: 0,
            entries: [kvm_cpuid_entry2::default(); CPUID_CAPACITY],
        }
    }

    fn entries(&self) -> Result<&[kvm_cpuid_entry2]> {
        self.entries
            .get(..self.count as usize)
            .context("invalid KVM TDX CPUID count")
    }
}

#[repr(C)]
struct Capabilities {
    attrs: u64,
    xfam: u64,
    reserved: [u64; 254],
    cpuid: Cpuid,
}

#[repr(C)]
struct Parameters {
    attrs: u64,
    xfam: u64,
    reserved: [u64; 30],
    cpuid: Cpuid,
}

#[repr(C)]
struct Import {
    address: u64,
    gpa: u64,
    pages: u64,
}

const _: () = assert!(size_of::<Command>() == 24);
const _: () = assert!(size_of::<kvm_cpuid_entry2>() == 40);
const _: () = assert!(std::mem::offset_of!(Capabilities, cpuid) == 2048);
const _: () = assert!(std::mem::offset_of!(Parameters, cpuid) == 256);
const _: () = assert!(size_of::<Import>() == 24);

pub struct Features {
    xfam: u64,
    cpuid_avx: u32,
}

pub fn create(kvm: &Kvm, count: u32) -> Result<(Arc<VmFd>, Features)> {
    let required = [
        (KVM_CAP_VM_TYPES, 1 << KVM_X86_TDX_VM, "KVM_X86_TDX_VM"),
        (
            KVM_CAP_MEMORY_ATTRIBUTES,
            KVM_MEMORY_ATTRIBUTE_PRIVATE,
            "private memory attributes",
        ),
        (KVM_CAP_GUEST_MEMFD, 1, "guest_memfd"),
        (KVM_CAP_SPLIT_IRQCHIP, 1, "split irqchip"),
        (KVM_CAP_SIGNAL_MSI, 1, "MSI injection"),
        (KVM_CAP_EXIT_HYPERCALL, 1 << MAP_GPA, "MapGPA exits"),
    ];
    for (capability, mask, name) in required {
        let value = kvm.check_extension_raw(capability.into());
        ensure!(
            value >= 0 && (value as u32 & mask) == mask,
            "KVM TDX requires {name}"
        );
    }
    let vm = Arc::new(
        kvm.create_vm_with_type(KVM_X86_TDX_VM.into())
            .context("KVM_CREATE_VM(TDX)")?,
    );
    let limit = vm.check_extension_raw(KVM_CAP_MAX_VCPUS.into());
    ensure!(limit >= 1, "cannot query the KVM TDX vCPU limit");
    ensure!(
        count <= limit as u32,
        "requested {count} vCPUs but this KVM TDX host supports {limit}"
    );

    let mut caps = Capabilities {
        attrs: 0,
        xfam: 0,
        reserved: [0; 254],
        cpuid: Cpuid::for_output(),
    };
    call(vm.as_raw_fd(), CAPABILITIES, 0, address_of(&mut caps))?;
    ensure!(
        caps.attrs & SEPT_VE_DISABLE != 0 && caps.xfam & XFAM_SSE == XFAM_SSE,
        "TDX host needs SEPT #VE disable and x87/SSE state"
    );
    let xfam = caps.xfam & (XFAM_SSE | XFAM_AVX);
    let mut cpuid_avx = 0;
    if xfam & XFAM_AVX != 0 {
        if let Some(entry) = caps
            .cpuid
            .entries()?
            .iter()
            .find(|entry| entry.function == 1)
        {
            cpuid_avx = entry.ecx & (CPUID_AVX | CPUID_F16C);
        }
    }
    Ok((vm, Features { xfam, cpuid_avx }))
}

pub fn enable_split_irqchip(vm: &VmFd) -> Result<()> {
    for (capability, value) in [
        (KVM_CAP_SPLIT_IRQCHIP, 24),
        (KVM_CAP_EXIT_HYPERCALL, 1 << MAP_GPA),
    ] {
        vm.enable_cap(&kvm_enable_cap {
            cap: capability,
            args: [value, 0, 0, 0],
            ..Default::default()
        })
        .with_context(|| format!("enable TDX KVM capability {capability}"))?;
    }
    Ok(())
}

pub fn launch(
    vm: &Arc<VmFd>,
    features: Features,
    count: u32,
    memory: &mut Memory,
    staged: Staged,
) -> Result<Vec<VcpuFd>> {
    let Staged {
        hob,
        measured,
        mut firmware,
    } = staged;
    let mut cpuid = Cpuid::with_count(1);
    // Select a 48-bit physical address width and four-level EPT.
    cpuid.entries[0] = kvm_cpuid_entry2 {
        function: 0x8000_0008,
        eax: 48 << 16,
        ..Default::default()
    };
    if features.cpuid_avx != 0 {
        cpuid.entries[cpuid.count as usize] = kvm_cpuid_entry2 {
            function: 1,
            ecx: features.cpuid_avx,
            ..Default::default()
        };
        cpuid.count += 1;
    }
    if count > 1 {
        for level in 0..2 {
            cpuid.entries[cpuid.count as usize] = topology(0x1f, count, level, 0);
            cpuid.count += 1;
        }
    }
    let mut params = Parameters {
        attrs: SEPT_VE_DISABLE,
        xfam: features.xfam,
        reserved: [0; 30],
        cpuid,
    };
    call(vm.as_raw_fd(), INIT_VM, 0, address_of(&mut params))?;

    let vcpus = (0..count)
        .map(|id| {
            launch_vcpu(vm, count, id, hob).with_context(|| format!("initialize TDX vCPU {id}"))
        })
        .collect::<Result<Vec<_>>>()?;
    firmware.register(vm, 1, true)?;

    let bsp = &vcpus[0];
    let mut cursor = 0;
    for (start, size) in measured {
        if size == 0 {
            continue;
        }
        ensure!(
            start >= cursor && start <= memory.low.size && size <= memory.low.size - start,
            "TDX measured memory range outside low RAM"
        );
        import(bsp, &memory.low, cursor, start - cursor, false)?;
        import(bsp, &memory.low, start, size, true)?;
        cursor = start + size;
    }
    import(bsp, &memory.low, cursor, memory.low.size - cursor, false)?;
    if let Some(high) = &memory.high {
        import(bsp, high, high.gpa, high.size, false)?;
    }
    import(bsp, &firmware, firmware.gpa, firmware.size, true)?;
    call(vm.as_raw_fd(), FINALIZE_VM, 0, 0)?;

    memory.low.discard(0, memory.low.size)?;
    if let Some(high) = &memory.high {
        high.discard(high.gpa, high.size)?;
    }
    memory.firmware = Some(firmware);
    eprintln!("INFO: TDX launch finalized (debug disabled)");
    Ok(vcpus)
}

fn launch_vcpu(vm: &VmFd, count: u32, id: u32, hob: u64) -> Result<VcpuFd> {
    let vcpu = vm.create_vcpu(id.into()).context("KVM_CREATE_VCPU")?;
    // TDX initialization requires x2APIC. Replace the bootstrap CPUID entry
    // with the module's architectural CPUID data once initialization completes.
    let bootstrap = CpuId::from_entries(&[kvm_cpuid_entry2 {
        function: 1,
        ecx: 1 << 21,
        ..Default::default()
    }])?;
    vcpu.set_cpuid2(&bootstrap)
        .context("enable x2APIC CPUID for TDX")?;
    call(vcpu.as_raw_fd(), INIT_VCPU, 0, hob)?;
    let mut cpuid = Cpuid::for_output();
    call(vcpu.as_raw_fd(), GET_CPUID, 0, address_of(&mut cpuid))?;
    let mut entries = cpuid.entries()?.to_vec();
    if count > 1 {
        // TD-Shim uses emulated leaf 0xb when APs enter the ACPI wakeup loop.
        entries.retain(|entry| entry.function != 0xb);
        entries.extend((0..3).map(|level| topology(0xb, count, level, id)));
    }
    ensure!(
        entries.len() + 2 <= CPUID_CAPACITY,
        "TDX CPUID table has no room for KVM leaves"
    );

    // The KVM signature enables x2APIC without IRQ remapping in Linux.
    // Keep the features leaf clear so Linux uses native TDX clock and IRQs.
    entries.push(kvm_cpuid_entry2 {
        function: KVM_SIGNATURE,
        eax: KVM_FEATURES,
        ebx: 0x4b4d_564b,
        ecx: 0x564b_4d56,
        edx: 0x4d,
        ..Default::default()
    });
    entries.push(kvm_cpuid_entry2 {
        function: KVM_FEATURES,
        ..Default::default()
    });
    let cpuid = CpuId::from_entries(&entries)?;
    vcpu.set_cpuid2(&cpuid).context("KVM_SET_CPUID2 for TDX")?;
    Ok(vcpu)
}

fn topology(leaf: u32, count: u32, level: u32, id: u32) -> kvm_cpuid_entry2 {
    let mut entry = kvm_cpuid_entry2 {
        function: leaf,
        index: level,
        flags: KVM_CPUID_FLAG_SIGNIFCANT_INDEX,
        ecx: level,
        edx: id,
        ..Default::default()
    };
    match level {
        0 => {
            entry.ebx = 1;
            entry.ecx |= 1 << 8;
        }
        1 => {
            entry.eax = u32::BITS - (count - 1).leading_zeros();
            entry.ebx = count;
            entry.ecx |= 2 << 8;
        }
        _ => {}
    }
    entry
}

fn import(vcpu: &VcpuFd, region: &Mapping, start: u64, size: u64, measure: bool) -> Result<()> {
    if size == 0 {
        return Ok(());
    }
    let mut range = Import {
        address: region.host_address(start - region.gpa),
        gpa: start,
        pages: size / PAGE_SIZE,
    };
    while range.pages != 0 {
        match call(
            vcpu.as_raw_fd(),
            INIT_MEM_REGION,
            u32::from(measure),
            address_of(&mut range),
        ) {
            Ok(()) => break,
            Err(error) => {
                if error
                    .downcast_ref::<io::Error>()
                    .and_then(io::Error::raw_os_error)
                    != Some(libc::EINTR)
                {
                    return Err(error);
                }
            }
        }
    }
    Ok(())
}

fn address_of<T>(value: &mut T) -> u64 {
    std::ptr::from_mut(value) as u64
}

fn call(fd: RawFd, number: u32, flags: u32, data: u64) -> Result<()> {
    let mut command = Command {
        number,
        flags,
        data,
        hardware_error: 0,
    };
    // SAFETY: The fd and ABI are selected by the caller. For pointer data,
    // the operation's command structure remains allocated through this call.
    if unsafe { libc::ioctl(fd, KVM_MEMORY_ENCRYPT_OP, &mut command) } < 0 {
        let errno = io::Error::last_os_error();
        let name = [
            "CAPABILITIES",
            "INIT_VM",
            "INIT_VCPU",
            "INIT_MEM_REGION",
            "FINALIZE_VM",
            "GET_CPUID",
        ][number as usize];
        return Err(errno).with_context(|| {
            format!(
                "KVM_TDX_{name} (hardware status {:#x})",
                command.hardware_error
            )
        });
    }
    Ok(())
}

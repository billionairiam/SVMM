use anyhow::{Context, Result, ensure};
use kvm_bindings::*;
use kvm_ioctls::{Cap, Kvm, VcpuFd, VmFd};
use std::{collections::BTreeMap, mem::size_of};

const REQUIRED: &[u32] = &[
    1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 30,
];
const MAX_ENTRIES: usize = 256;

// Only fixed, integer-only KVM UAPI structures with initialized padding are
// copied byte-for-byte into the existing SVMMVMST v1 format.
trait Uapi {}
macro_rules! uapi {
    ($($type:ty),* $(,)?) => { $(impl Uapi for $type {})* };
}
uapi!(
    u32,
    kvm_irqchip,
    kvm_pit_state2,
    kvm_clock_data,
    kvm_cpuid_entry2,
    kvm_mp_state,
    kvm_regs,
    kvm_sregs,
    kvm_xsave,
    kvm_xcrs,
    kvm_debugregs,
    kvm_lapic_state,
    kvm_msr_entry,
    kvm_vcpu_events,
);

fn bytes<T: Uapi>(value: &T) -> &[u8] {
    // These UAPI fields are integer bit patterns, including reserved storage.
    unsafe { std::slice::from_raw_parts(std::ptr::from_ref(value).cast::<u8>(), size_of::<T>()) }
}

fn value<T: Uapi>(data: &[u8]) -> Result<T> {
    ensure!(data.len() == size_of::<T>(), "bad vmstate section length");
    Ok(unsafe { data.as_ptr().cast::<T>().read_unaligned() })
}

fn check_array<T: Uapi>(data: &[u8]) -> Result<()> {
    ensure!(data.len() >= 8, "bad vmstate array header");
    let count = u32::from_le_bytes(data[..4].try_into()?) as usize;
    ensure!(
        count <= MAX_ENTRIES && data.len() == 8 + count * size_of::<T>(),
        "bad vmstate array length"
    );
    Ok(())
}

fn array<T: Uapi>(data: &[u8]) -> Result<Vec<T>> {
    check_array::<T>(data)?;
    data[8..].chunks_exact(size_of::<T>()).map(value).collect()
}

fn array_bytes<T: Uapi>(values: &[T]) -> Result<Vec<u8>> {
    ensure!(
        values.len() <= MAX_ENTRIES,
        "unsupported number of vmstate entries"
    );
    let mut result = (values.len() as u64).to_le_bytes().to_vec();
    for entry in values {
        result.extend_from_slice(bytes(entry));
    }
    Ok(result)
}

fn msrs(vcpu: &VcpuFd) -> Result<Vec<kvm_msr_entry>> {
    let indices = Kvm::new()?.get_msr_index_list()?;
    let mut pending = indices.as_slice();
    let mut output = Vec::new();
    while !pending.is_empty() {
        let entries: Vec<_> = pending
            .iter()
            .map(|&index| kvm_msr_entry {
                index,
                ..Default::default()
            })
            .collect();
        let mut request =
            Msrs::from_entries(&entries).map_err(|_| anyhow::anyhow!("MSR list is too long"))?;
        let count = vcpu.get_msrs(&mut request)?;
        output.extend_from_slice(&request.as_slice()[..count]);
        pending = &pending[(count + 1).min(pending.len())..];
    }
    Ok(output)
}

fn check_xsave(vm: &VmFd) -> Result<()> {
    ensure!(
        vm.check_extension_int(Cap::Xsave2) <= size_of::<kvm_xsave>() as i32,
        "guest XSAVE exceeds snapshot format"
    );
    Ok(())
}

pub fn capture(vm: &VmFd, vcpu: &VcpuFd, devices: &[(u32, Vec<u8>)]) -> Result<Vec<u8>> {
    check_xsave(vm)?;
    let mut sections = Vec::new();
    for chip_id in [0, 1, 2] {
        let mut chip = kvm_irqchip {
            chip_id,
            ..Default::default()
        };
        vm.get_irqchip(&mut chip).context("get irqchip")?;
        sections.push((chip_id + 1, bytes(&chip).to_vec()));
    }
    sections.extend([
        (4, bytes(&vm.get_pit2()?).to_vec()),
        (5, bytes(&vm.get_clock()?).to_vec()),
        (10, array_bytes(vcpu.get_cpuid2(MAX_ENTRIES)?.as_slice())?),
        (11, bytes(&vcpu.get_tsc_khz()?).to_vec()),
        (12, bytes(&vcpu.get_mp_state()?).to_vec()),
        (13, bytes(&vcpu.get_regs()?).to_vec()),
        (14, bytes(&vcpu.get_sregs()?).to_vec()),
        (15, bytes(&vcpu.get_xsave()?).to_vec()),
        (16, bytes(&vcpu.get_xcrs()?).to_vec()),
        (17, bytes(&vcpu.get_debug_regs()?).to_vec()),
        (18, bytes(&vcpu.get_lapic()?).to_vec()),
        (19, array_bytes(&msrs(vcpu)?)?),
        (20, bytes(&vcpu.get_vcpu_events()?).to_vec()),
    ]);
    sections.extend_from_slice(devices);
    let mut encoded = b"SVMMVMST".to_vec();
    encoded.extend_from_slice(&1u32.to_le_bytes());
    encoded.extend_from_slice(&(sections.len() as u32).to_le_bytes());
    for (tag, data) in sections {
        encoded.extend_from_slice(&tag.to_le_bytes());
        encoded.extend_from_slice(&(data.len() as u32).to_le_bytes());
        encoded.extend_from_slice(&data);
    }
    Ok(encoded)
}

pub struct State<'a>(BTreeMap<u32, &'a [u8]>);

impl<'a> State<'a> {
    pub fn decode(input: &'a [u8], disk: bool, net: bool) -> Result<Self> {
        ensure!(
            input.len() >= 16 && &input[..8] == b"SVMMVMST",
            "vmstate: bad magic"
        );
        ensure!(
            u32::from_le_bytes(input[8..12].try_into()?) == 1,
            "vmstate: unsupported version"
        );
        let count = u32::from_le_bytes(input[12..16].try_into()?);
        ensure!(
            count as usize <= REQUIRED.len() + 2,
            "vmstate: too many sections"
        );
        let mut segments = &input[16..];
        let mut output = BTreeMap::new();
        for _ in 0..count {
            ensure!(segments.len() >= 8, "vmstate: truncated section header");
            let tag = u32::from_le_bytes(segments[..4].try_into()?);
            let size = u32::from_le_bytes(segments[4..8].try_into()?) as usize;
            ensure!(
                REQUIRED.contains(&tag) || matches!(tag, 31 | 32),
                "vmstate: unknown section tag {tag}"
            );
            ensure!(
                segments.len() - 8 >= size,
                "vmstate: truncated section {tag}"
            );
            let (payload, rest) = segments[8..].split_at(size);
            ensure!(
                output.insert(tag, payload).is_none(),
                "vmstate: duplicate section {tag}"
            );
            segments = rest;
        }
        ensure!(segments.is_empty(), "vmstate: trailing bytes");
        for tag in REQUIRED {
            ensure!(output.contains_key(tag), "vmstate: missing section {tag}");
        }
        ensure!(
            output.contains_key(&31) == disk,
            "vmstate: virtio-blk device mismatch"
        );
        ensure!(
            output.contains_key(&32) == net,
            "vmstate: virtio-net device mismatch"
        );
        let state = Self(output);
        for tag in 1..=3 {
            ensure!(
                state.get::<kvm_irqchip>(tag)?.chip_id == tag - 1,
                "vmstate: wrong irqchip ID"
            );
        }
        check_array::<kvm_cpuid_entry2>(state.0[&10]).context("vmstate: CPUID")?;
        check_array::<kvm_msr_entry>(state.0[&19]).context("vmstate: MSRs")?;
        for (tag, size) in [
            (4, size_of::<kvm_pit_state2>()),
            (5, size_of::<kvm_clock_data>()),
            (11, size_of::<u32>()),
            (12, size_of::<kvm_mp_state>()),
            (13, size_of::<kvm_regs>()),
            (14, size_of::<kvm_sregs>()),
            (15, size_of::<kvm_xsave>()),
            (16, size_of::<kvm_xcrs>()),
            (17, size_of::<kvm_debugregs>()),
            (18, size_of::<kvm_lapic_state>()),
            (20, size_of::<kvm_vcpu_events>()),
            (30, 4112),
            (31, 192),
            (32, 192),
        ] {
            if let Some(section) = state.0.get(&tag) {
                ensure!(
                    section.len() == size,
                    "vmstate: wrong length for section {tag}"
                );
            }
        }
        Ok(state)
    }

    fn get<T: Uapi>(&self, tag: u32) -> Result<T> {
        value(self.0[&tag]).with_context(|| format!("vmstate section {tag}"))
    }

    pub fn devices(&self) -> Vec<(u32, Vec<u8>)> {
        self.0
            .range(30..=32)
            .map(|(&tag, data)| (tag, data.to_vec()))
            .collect()
    }

    pub fn apply(&self, vm: &VmFd, vcpu: &VcpuFd) -> Result<()> {
        check_xsave(vm)?;
        let saved_frequency = self.get::<u32>(11)?;
        ensure!(
            vcpu.get_tsc_khz()? == saved_frequency,
            "vmstate: TSC frequency differs from snapshot"
        );
        for tag in 1..=3 {
            vm.set_irqchip(&self.get(tag)?)?;
        }
        vm.set_pit2(&self.get(4)?)?;
        let mut clock: kvm_clock_data = self.get(5)?;
        clock.flags = 0;
        vm.set_clock(&clock)?;
        vcpu.set_mp_state(self.get(12)?)?;
        vcpu.set_regs(&self.get(13)?)?;
        vcpu.set_sregs(&self.get(14)?)?;
        // Legacy xsave is fixed-size, checked above against KVM_CAP_XSAVE2.
        unsafe { vcpu.set_xsave(&self.get(15)?)? };
        vcpu.set_xcrs(&self.get(16)?)?;
        vcpu.set_debug_regs(&self.get(17)?)?;
        vcpu.set_lapic(&self.get(18)?)?;
        let entries = array::<kvm_msr_entry>(self.0[&19])?;
        let msrs = Msrs::from_entries(&entries).map_err(|_| anyhow::anyhow!("invalid MSR list"))?;
        ensure!(
            vcpu.set_msrs(&msrs)? == entries.len(),
            "vmstate: KVM rejected an MSR in the snapshot"
        );
        let mut events: kvm_vcpu_events = self.get(20)?;
        events.flags |= KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SIPI_VECTOR;
        vcpu.set_vcpu_events(&events)?;
        let cpuid = array::<kvm_cpuid_entry2>(self.0[&10])?;
        ensure!(
            vcpu.get_cpuid2(MAX_ENTRIES)?.as_slice() == cpuid,
            "vmstate: CPUID differs from snapshot"
        );
        Ok(())
    }
}

use std::fs::File;
use std::path::Path;
use std::sync::Arc;

use anyhow::{Context, Result, ensure};
use kvm_bindings::{
    KVM_API_VERSION, KVM_MAX_CPUID_ENTRIES, KVM_PIT_SPEAKER_DUMMY, kvm_pit_config, kvm_regs,
    kvm_segment,
};
use kvm_ioctls::{Cap, Kvm, VcpuFd, VmFd};

use crate::memory::{LOW_RAM_LIMIT, MIB, Memory};

#[path = "boot/mod.rs"]
mod boot;

use boot::{acpi, linux, td_shim, tdx};

pub struct BootConfig<'a> {
    pub kernel: &'a Path,
    pub initrd: Option<&'a Path>,
    pub cmdline: &'a str,
    pub mem_mib: usize,
    pub vcpus: u32,
    pub td_shim: Option<&'a Path>,
    pub disk: bool,
    pub net: bool,
}

pub struct Machine {
    pub vm: Arc<VmFd>,
    pub vcpus: Vec<VcpuFd>,
    pub memory: Arc<Memory>,
}

impl Machine {
    pub fn boot(config: &BootConfig<'_>) -> Result<Self> {
        let is_tdx = config.td_shim.is_some();
        let ram_size = ram_size(config.mem_mib, is_tdx)?;
        validate_cpu_count(config.vcpus, is_tdx)?;
        let image = stage_result(linux::Image::open(config.kernel), "image_load", 0)?;
        metric("image_loaded", 0);
        let tables = acpi::Tables::new(config.vcpus, config.disk, config.net, is_tdx);

        let (vm, vcpus, memory) = if let Some(path) = config.td_shim {
            let shim = stage_result(td_shim::Shim::open(path), "tdx_init", 0)?;
            let kvm = stage_result(open_kvm(), "tdx_init", 0)?;
            let (vm, features) = stage_result(tdx::create(&kvm, config.vcpus), "tdx_init", 0)?;
            stage_result(tdx::enable_split_irqchip(&vm), "tdx_init", 0)?;
            let mut memory = map_ram(&vm, ram_size, true, None)?;
            stage_result(tables.stage(&memory), "acpi_setup", ram_size)?;
            let initrd = prepare_initrd(config, &image, &memory, ram_size)?;
            let staged = stage_result(
                shim.stage(&memory, &tables, &image, &initrd, config.cmdline),
                "tdx_boot",
                ram_size,
            )?;
            let vcpus = stage_result(
                tdx::launch(&vm, features, config.vcpus, &mut memory, staged),
                "tdx_boot",
                ram_size,
            )?;
            (vm, vcpus, memory)
        } else {
            let kvm = stage_result(open_kvm(), "kvm_init", 0)?;
            let vm = stage_result(create_plain_vm(&kvm), "kvm_init", 0)?;
            let memory = map_ram(&vm, ram_size, false, None)?;
            stage_result(tables.stage(&memory), "acpi_setup", ram_size)?;
            let initrd = prepare_initrd(config, &image, &memory, ram_size)?;
            stage_result(
                image.stage(&memory, config.cmdline, &initrd),
                "boot_prepare",
                ram_size,
            )?;
            metric("boot_prepared", ram_size);
            let vcpu = stage_result(create_plain_vcpu(&kvm, &vm), "vcpu_init", ram_size)?;
            stage_result(linux_registers(&vcpu), "vcpu_setup", ram_size)?;
            (vm, vec![vcpu], memory)
        };
        metric("vcpu_setup", ram_size);
        Ok(Self {
            vm,
            vcpus,
            memory: Arc::new(memory),
        })
    }

    pub fn restore(mem_mib: usize, memory_file: &File) -> Result<Self> {
        let ram_size = ram_size(mem_mib, false)?;
        let kvm = open_kvm()?;
        let vm = create_plain_vm(&kvm)?;
        let memory = map_ram(&vm, ram_size, false, Some(memory_file))?;
        let vcpu = create_plain_vcpu(&kvm, &vm)?;
        Ok(Self {
            vm,
            vcpus: vec![vcpu],
            memory: Arc::new(memory),
        })
    }
}

pub fn tdx_preflight(path: &Path, vcpus: u32) -> Result<()> {
    validate_cpu_count(vcpus, true)?;
    td_shim::Shim::open(path)?;
    tdx::create(&open_kvm()?, vcpus)?;
    Ok(())
}

fn prepare_initrd(
    config: &BootConfig<'_>,
    image: &linux::Image,
    memory: &Memory,
    ram_size: u64,
) -> Result<linux::Initrd> {
    if let Some(path) = config.initrd.filter(|path| !path.as_os_str().is_empty()) {
        stage_result(image.load_initrd(path, memory), "initramfs_load", ram_size)
    } else {
        Ok(linux::Initrd::default())
    }
}

fn ram_size(mem_mib: usize, tdx: bool) -> Result<u64> {
    let maximum = if tdx {
        32 * 1024
    } else {
        (LOW_RAM_LIMIT / MIB) as usize
    };
    ensure!(
        (64..=maximum).contains(&mem_mib),
        "guest memory must be 64-{maximum} MiB, got {mem_mib} MiB"
    );
    Ok(mem_mib as u64 * MIB)
}

fn validate_cpu_count(vcpus: u32, tdx: bool) -> Result<()> {
    if tdx {
        ensure!((1..=64).contains(&vcpus), "TDX requires 1-64 vCPUs");
    } else {
        ensure!(vcpus == 1, "ordinary KVM requires one vCPU");
    }
    Ok(())
}

fn open_kvm() -> Result<Kvm> {
    let kvm = Kvm::new().context("open /dev/kvm")?;
    let version = kvm.get_api_version();
    ensure!(
        version == KVM_API_VERSION as i32,
        "unsupported KVM API version: {version}"
    );
    Ok(kvm)
}

fn create_plain_vm(kvm: &Kvm) -> Result<Arc<VmFd>> {
    let vm = kvm.create_vm().context("KVM_CREATE_VM")?;
    if let Err(error) = vm.set_tss_address(0xfffb_d000) {
        eprintln!("WARN: KVM_SET_TSS_ADDR: {error}");
    }
    vm.create_irq_chip().context("KVM_CREATE_IRQCHIP")?;
    vm.create_pit2(kvm_pit_config {
        flags: KVM_PIT_SPEAKER_DUMMY,
        ..Default::default()
    })
    .context("KVM_CREATE_PIT2")?;
    Ok(Arc::new(vm))
}

fn map_ram(vm: &Arc<VmFd>, size: u64, tdx: bool, image: Option<&File>) -> Result<Memory> {
    metric("memory", size);
    let memory = stage_result(Memory::new(vm, size, tdx, image), "guest_memory", size)?;
    metric("memory_mapped", size);
    Ok(memory)
}

fn create_plain_vcpu(kvm: &Kvm, vm: &VmFd) -> Result<VcpuFd> {
    let vcpu = vm.create_vcpu(0).context("KVM_CREATE_VCPU")?;
    let mut cpuid = kvm
        .get_supported_cpuid(KVM_MAX_CPUID_ENTRIES)
        .context("KVM_GET_SUPPORTED_CPUID")?;
    let tsc_deadline = kvm.check_extension(Cap::TscDeadlineTimer);
    for entry in cpuid.as_mut_slice() {
        match entry.function {
            1 => {
                entry.ebx &= 0x00ff_ffff;
                if tsc_deadline {
                    entry.ecx |= 1 << 24;
                }
            }
            0xb | 0x1f => entry.edx = 0,
            _ => {}
        }
    }
    vcpu.set_cpuid2(&cpuid).context("KVM_SET_CPUID2")?;
    Ok(vcpu)
}

fn linux_registers(vcpu: &VcpuFd) -> Result<()> {
    let mut sregs = vcpu.get_sregs().context("KVM_GET_SREGS")?;
    let code = kvm_segment {
        limit: u32::MAX,
        selector: linux::CODE_SEGMENT,
        type_: 0xb,
        present: 1,
        s: 1,
        db: 1,
        g: 1,
        ..Default::default()
    };
    let data = kvm_segment {
        selector: linux::DATA_SEGMENT,
        type_: 0x3,
        ..code
    };
    sregs.cs = code;
    sregs.ds = data;
    sregs.es = data;
    sregs.fs = data;
    sregs.gs = data;
    sregs.ss = data;
    sregs.gdt.base = linux::GDT_ADDR;
    sregs.gdt.limit = 31;
    sregs.cr0 = (sregs.cr0 | 1) & !(1 << 31);
    sregs.cr3 = 0;
    sregs.cr4 = 0;
    sregs.efer = 0;
    vcpu.set_sregs(&sregs).context("KVM_SET_SREGS")?;
    vcpu.set_regs(&kvm_regs {
        rip: linux::KERNEL_ADDR,
        rflags: 2,
        rsi: linux::PARAMS_ADDR,
        rsp: linux::STACK_ADDR,
        ..Default::default()
    })
    .context("KVM_SET_REGS")?;
    Ok(())
}

fn stage_result<T>(result: Result<T>, stage: &str, size: u64) -> Result<T> {
    if result.is_err() {
        metric(&format!("failed_{stage}"), size);
    }
    result
}

fn metric(stage: &str, size: u64) {
    eprintln!("SVMM_METRIC variant=baseline stage={stage} guest_memory_bytes={size}");
}

use std::fs::File;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU16, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};

use anyhow::{Context, Result, bail, ensure};
use kvm_bindings::{
    KVM_MEM_GUEST_MEMFD, KVM_MEMORY_ATTRIBUTE_PRIVATE, kvm_create_guest_memfd,
    kvm_memory_attributes, kvm_userspace_memory_region, kvm_userspace_memory_region2,
};
use kvm_ioctls::VmFd;

pub const MIB: u64 = 1024 * 1024;
pub const PAGE_SIZE: u64 = 4096;
pub const HIGH_RAM_BASE: u64 = 1 << 32;
pub const LOW_RAM_LIMIT: u64 = 3072 * MIB;
pub const TDX_GPA_SHARED: u64 = 1 << 47;
const MAP_PRIVATE: u64 = 0x10;

pub struct Mapping {
    pointer: NonNull<u8>,
    pub gpa: u64,
    pub size: u64,
    slot: Option<(Arc<VmFd>, u32)>,
    private: Option<OwnedFd>,
}

// KVM and DMA use the same mapped memory. Host-side copies are controlled by Memory.
// Regular reads and writes use copies, with only aligned ring indices accessed atomically.
unsafe impl Send for Mapping {}
unsafe impl Sync for Mapping {}

impl Mapping {
    pub fn new(gpa: u64, size: u64, file: Option<&File>) -> Result<Self> {
        let (fd, flags) = match file {
            Some(file) => (file.as_raw_fd(), libc::MAP_PRIVATE),
            None => (-1, libc::MAP_PRIVATE | libc::MAP_ANONYMOUS),
        };
        // SAFETY: The file is either a checked RAM image or this is an anonymous map.
        let pointer = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size as usize,
                libc::PROT_READ | libc::PROT_WRITE,
                flags,
                fd,
                0,
            )
        };
        if pointer == libc::MAP_FAILED {
            return Err(io::Error::last_os_error()).context("mmap guest memory");
        }
        let Some(pointer) = NonNull::new(pointer.cast()) else {
            // SAFETY: Clean up the mapping just returned by mmap.
            unsafe { libc::munmap(pointer, size as usize) };
            bail!("mmap returned an unusable null address");
        };
        Ok(Self {
            pointer,
            gpa,
            size,
            slot: None,
            private: None,
        })
    }

    pub fn host_address(&self, offset: u64) -> u64 {
        assert!(offset < self.size);
        // SAFETY: The bounds check keeps this address inside the owned mapping.
        unsafe { self.pointer.as_ptr().add(offset as usize) as u64 }
    }

    fn contains(&self, gpa: u64, size: u64) -> bool {
        gpa.checked_sub(self.gpa)
            .is_some_and(|offset| offset <= self.size && size <= self.size - offset)
    }

    pub fn register(&mut self, vm: &Arc<VmFd>, slot: u32, private: bool) -> Result<()> {
        if private {
            let fd = vm
                .create_guest_memfd(kvm_create_guest_memfd {
                    size: self.size,
                    ..Default::default()
                })
                .context("KVM_CREATE_GUEST_MEMFD")?;
            // SAFETY: A successful ioctl returns a new, owned guest_memfd.
            let fd = unsafe { OwnedFd::from_raw_fd(fd) };
            let region = kvm_userspace_memory_region2 {
                slot,
                flags: KVM_MEM_GUEST_MEMFD,
                guest_phys_addr: self.gpa,
                memory_size: self.size,
                userspace_addr: self.host_address(0),
                guest_memfd: fd.as_raw_fd() as u32,
                ..Default::default()
            };
            // SAFETY: Drop unregisters this KVM slot before freeing its mapping.
            unsafe { vm.set_user_memory_region2(region) }.context("KVM_SET_USER_MEMORY_REGION2")?;
            self.private = Some(fd);
            self.slot = Some((Arc::clone(vm), slot));
            set_private(vm, self.gpa, self.size, true)?;
        } else {
            let region = kvm_userspace_memory_region {
                slot,
                flags: 0,
                guest_phys_addr: self.gpa,
                memory_size: self.size,
                userspace_addr: self.host_address(0),
            };
            // SAFETY: Drop unregisters this KVM slot before freeing its mapping.
            unsafe { vm.set_user_memory_region(region) }.context("KVM_SET_USER_MEMORY_REGION")?;
            self.slot = Some((Arc::clone(vm), slot));
        }
        Ok(())
    }

    pub fn discard(&self, gpa: u64, size: u64) -> Result<()> {
        ensure!(self.contains(gpa, size), "discard outside guest mapping");
        // SAFETY: The range belongs to the mapping and callers serialize against DMA.
        if unsafe {
            libc::madvise(
                self.host_address(gpa - self.gpa) as *mut libc::c_void,
                size as usize,
                libc::MADV_DONTNEED,
            )
        } != 0
        {
            return Err(io::Error::last_os_error()).context("discard TDX staging memory");
        }
        Ok(())
    }

    pub fn write(&self, gpa: u64, source: &[u8]) -> Result<()> {
        ensure!(
            self.contains(gpa, source.len() as u64),
            "write outside guest mapping"
        );
        if source.is_empty() {
            return Ok(());
        }
        let address = self.host_address(gpa - self.gpa);
        // SAFETY: The source is a disjoint host buffer and its length fits in the map.
        unsafe { std::ptr::copy_nonoverlapping(source.as_ptr(), address as *mut u8, source.len()) };
        Ok(())
    }

    fn read(&self, gpa: u64, destination: &mut [u8]) -> Result<()> {
        ensure!(
            self.contains(gpa, destination.len() as u64),
            "read outside guest mapping"
        );
        if destination.is_empty() {
            return Ok(());
        }
        let address = self.host_address(gpa - self.gpa);
        // SAFETY: The destination is a disjoint host buffer and its length fits in the map.
        unsafe {
            std::ptr::copy_nonoverlapping(
                address as *const u8,
                destination.as_mut_ptr(),
                destination.len(),
            )
        };
        Ok(())
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        if let Some((vm, slot)) = &self.slot {
            // SAFETY: A zero-length region unregisters a slot without referencing any memory.
            let result = unsafe {
                if self.private.is_some() {
                    vm.set_user_memory_region2(kvm_userspace_memory_region2 {
                        slot: *slot,
                        ..Default::default()
                    })
                } else {
                    vm.set_user_memory_region(kvm_userspace_memory_region {
                        slot: *slot,
                        ..Default::default()
                    })
                }
            };
            if let Err(error) = result {
                // Preserve the backing while KVM could still reference the slot.
                eprintln!("WARN: unregister guest memory slot {slot}: {error}");
                return;
            }
        }
        // SAFETY: KVM no longer references the mapping, and the last Memory owner is dropping.
        unsafe { libc::munmap(self.pointer.as_ptr().cast(), self.size as usize) };
    }
}

fn set_private(vm: &VmFd, gpa: u64, size: u64, private: bool) -> Result<()> {
    vm.set_memory_attributes(kvm_memory_attributes {
        address: gpa,
        size,
        attributes: if private {
            u64::from(KVM_MEMORY_ATTRIBUTE_PRIVATE)
        } else {
            0
        },
        flags: 0,
    })
    .context("KVM_SET_MEMORY_ATTRIBUTES")
}

pub struct Memory {
    pub(crate) low: Mapping,
    pub(crate) high: Option<Mapping>,
    pub(crate) firmware: Option<Mapping>,
    shared: Mutex<Option<Vec<bool>>>,
}

#[derive(Debug)]
pub struct InvalidMapGpa;

impl std::fmt::Display for InvalidMapGpa {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("invalid MapGPA request")
    }
}

impl std::error::Error for InvalidMapGpa {}

impl Memory {
    pub(crate) fn new(vm: &Arc<VmFd>, size: u64, tdx: bool, file: Option<&File>) -> Result<Self> {
        if let Some(file) = file {
            let metadata = file.metadata().context("stat warm snapshot memory")?;
            ensure!(
                metadata.is_file() && metadata.len() == size,
                "guest memory file must be a regular file of {} MiB",
                size / MIB
            );
        }
        let low_size = size.min(LOW_RAM_LIMIT);
        let mut low = Mapping::new(0, low_size, file)?;
        low.register(vm, 0, tdx)?;
        let high = if size > low_size {
            let mut mapping = Mapping::new(HIGH_RAM_BASE, size - low_size, None)?;
            mapping.register(vm, 2, tdx)?;
            Some(mapping)
        } else {
            None
        };
        let max_gpa = high.as_ref().map_or(low_size, |high| high.gpa + high.size);
        Ok(Self {
            low,
            high,
            firmware: None,
            shared: Mutex::new(tdx.then(|| vec![false; (max_gpa / PAGE_SIZE) as usize])),
        })
    }

    fn ram(&self, gpa: u64, size: u64) -> Option<&Mapping> {
        if self.low.contains(gpa, size) {
            Some(&self.low)
        } else {
            self.high
                .as_ref()
                .filter(|region| region.contains(gpa, size))
        }
    }

    pub(crate) fn stage(&self, gpa: u64, source: &[u8]) -> Result<()> {
        self.low.write(gpa, source)
    }

    pub(crate) fn load(&self, mut gpa: u64, mut size: usize, input: &mut dyn Read) -> Result<()> {
        ensure!(
            self.low.contains(gpa, size as u64),
            "image outside guest RAM"
        );
        let mut buffer = vec![0; 64 * 1024];
        while size != 0 {
            let bytes = size.min(buffer.len());
            input.read_exact(&mut buffer[..bytes])?;
            self.stage(gpa, &buffer[..bytes])?;
            gpa += bytes as u64;
            size -= bytes;
        }
        Ok(())
    }

    pub fn dma(&self) -> Dma<'_> {
        Dma {
            memory: self,
            shared: self
                .shared
                .lock()
                .unwrap_or_else(|poison| poison.into_inner()),
        }
    }

    pub fn convert(&self, vm: &VmFd, gpa: u64, pages: u64, mode: u64) -> Result<()> {
        let size = pages.checked_mul(PAGE_SIZE).ok_or(InvalidMapGpa)?;
        ensure!(
            gpa % PAGE_SIZE == 0 && pages != 0 && matches!(mode, 0 | MAP_PRIVATE),
            InvalidMapGpa
        );
        let ram = self.ram(gpa, size).ok_or(InvalidMapGpa)?;
        let fd = ram.private.as_ref().ok_or(InvalidMapGpa)?;
        let mut shared = self
            .shared
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let shared = shared.as_mut().ok_or(InvalidMapGpa)?;
        let first = (gpa / PAGE_SIZE) as usize;
        let page_range = &mut shared[first..first + pages as usize];
        let private = mode == MAP_PRIVATE;

        // Keep DMA access disabled if one half of the transition fails.
        page_range.fill(false);
        set_private(vm, gpa, size, private)?;
        if private {
            ram.discard(gpa, size)?;
        } else {
            let offset = (gpa - ram.gpa) as libc::off_t;
            // SAFETY: The checked range lies in this guest_memfd.
            if unsafe {
                libc::fallocate(
                    fd.as_raw_fd(),
                    libc::FALLOC_FL_PUNCH_HOLE | libc::FALLOC_FL_KEEP_SIZE,
                    offset,
                    size as libc::off_t,
                )
            } != 0
            {
                return Err(io::Error::last_os_error()).context("release TDX private pages");
            }
        }
        page_range.fill(!private);
        Ok(())
    }

    pub fn dump(&self, output: &mut dyn Write) -> Result<()> {
        let dma = self.dma();
        ensure!(dma.shared.is_none(), "cannot snapshot TDX guest memory");
        let mut buffer = vec![0; 64 * 1024];
        let mut gpa = 0;
        while gpa < self.low.size {
            let bytes = (self.low.size - gpa).min(buffer.len() as u64) as usize;
            dma.read(gpa, &mut buffer[..bytes])?;
            output.write_all(&buffer[..bytes])?;
            gpa += bytes as u64;
        }
        Ok(())
    }
}

pub struct Dma<'a> {
    memory: &'a Memory,
    shared: MutexGuard<'a, Option<Vec<bool>>>,
}

impl Dma<'_> {
    fn translate(&self, address: u64, len: usize) -> Result<(&Mapping, u64)> {
        let gpa = if self.shared.is_some() {
            address & !TDX_GPA_SHARED
        } else {
            address
        };
        let size = if self.shared.is_some() {
            len.max(1)
        } else {
            len
        } as u64;
        let region = self
            .memory
            .ram(gpa, size)
            .context("DMA range outside guest RAM")?;
        if let Some(shared) = self.shared.as_ref() {
            let first = (gpa / PAGE_SIZE) as usize;
            let last = ((gpa + size.saturating_sub(1)) / PAGE_SIZE) as usize;
            ensure!(
                shared[first..=last].iter().all(|page| *page),
                "DMA range includes private TDX memory"
            );
        }
        Ok((region, gpa))
    }

    pub fn read_u16_acquire(&self, address: u64) -> Result<u16> {
        let index = self.u16_index(address)?;
        Ok(u16::from_le(index.load(Ordering::Acquire)))
    }

    pub fn write_u16_release(&self, address: u64, value: u16) -> Result<()> {
        let index = self.u16_index(address)?;
        index.store(value.to_le(), Ordering::Release);
        Ok(())
    }

    fn u16_index(&self, address: u64) -> Result<&AtomicU16> {
        let (region, gpa) = self.translate(address, size_of::<u16>())?;
        let pointer = region.host_address(gpa - region.gpa);
        ensure!(
            pointer % align_of::<AtomicU16>() as u64 == 0,
            "unaligned DMA index"
        );
        // SAFETY: The address is validated, naturally aligned, and the DMA guard
        // keeps the page accessible for the lifetime of the atomic reference.
        Ok(unsafe { AtomicU16::from_ptr(pointer as *mut u16) })
    }

    pub fn check(&self, address: u64, len: usize) -> Result<()> {
        self.translate(address, len).map(|_| ())
    }

    pub fn read(&self, address: u64, destination: &mut [u8]) -> Result<()> {
        let (region, gpa) = self.translate(address, destination.len())?;
        region.read(gpa, destination)
    }

    pub fn write(&self, address: u64, source: &[u8]) -> Result<()> {
        let (region, gpa) = self.translate(address, source.len())?;
        region.write(gpa, source)
    }
}

pub(super) mod acpi;
pub(super) mod linux;
pub(super) mod td_shim;
pub(super) mod tdx;

use std::fs::{File, OpenOptions};
use std::io::Read;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

use anyhow::{Context, Result, ensure};

fn open_image(path: &Path, kind: &str) -> Result<(File, u64)> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .with_context(|| format!("open {kind}: {}", path.display()))?;
    let metadata = file.metadata().with_context(|| format!("stat {kind}"))?;
    ensure!(
        metadata.is_file(),
        "invalid {kind} file: {}",
        path.display()
    );
    Ok((file, metadata.len()))
}

fn read_image(path: &Path, kind: &str, min: u64, max: u64) -> Result<Vec<u8>> {
    let (mut file, size) = open_image(path, kind)?;
    ensure!(
        (min..=max).contains(&size),
        "invalid {kind} image size: {}",
        path.display()
    );
    let mut bytes = vec![0; size as usize];
    file.read_exact(&mut bytes)
        .with_context(|| format!("read {kind}"))?;
    Ok(bytes)
}

fn get_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap())
}

fn get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn get_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn align_up(value: u64, alignment: u64) -> u64 {
    (value + alignment - 1) & !(alignment - 1)
}

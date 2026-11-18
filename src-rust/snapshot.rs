use anyhow::{Context, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use std::{
    ffi::CString,
    fs::{self, File, OpenOptions},
    io::{self, Read, Seek, SeekFrom, Write},
    os::{
        fd::AsRawFd,
        unix::{
            ffi::OsStrExt,
            fs::{FileExt, OpenOptionsExt, PermissionsExt},
        },
    },
    path::{Path, PathBuf},
};

const MIB: usize = 1024 * 1024;
const PAGE: usize = 4096;
const FNV_OFFSET: u64 = 0xcbf29ce484222325;
const FNV_PRIME: u64 = 0x100000001b3;

#[derive(Default)]
pub struct Info {
    pub kernel: Option<String>,
    pub initrd: Option<String>,
    pub cmdline: Option<String>,
    pub mem: Option<usize>,
}

#[derive(Deserialize, Serialize)]
struct Manifest {
    manifest_version: u32,
    snapshot_type: String,
    name: String,
    created_at: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    disk_filename: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    disk_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    disk_size_bytes: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    disk_fnv1a64: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    guest_mem_mb: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    memory_filename: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    memory_size_bytes: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    memory_hash: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    vmstate_filename: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    vmstate_size_bytes: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    vmstate_fnv1a64: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    disk: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    net: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    kernel_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    initrd_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    cmdline: Option<String>,
}

fn root() -> PathBuf {
    std::env::var_os("SVMM_SNAPSHOT_DIR")
        .filter(|s| !s.is_empty())
        .map_or_else(|| PathBuf::from("snapshots"), PathBuf::from)
}

fn directory(name: &str) -> Result<PathBuf> {
    ensure!(
        crate::cli::identifier(name, 128, false),
        "invalid snapshot name '{name}'"
    );
    Ok(root().join(name))
}

fn created_at() -> Result<String> {
    let now = unsafe { libc::time(std::ptr::null_mut()) };
    let mut tm = std::mem::MaybeUninit::uninit();
    ensure!(
        !unsafe { libc::gmtime_r(&now, tm.as_mut_ptr()) }.is_null(),
        "convert snapshot timestamp"
    );
    let mut result = [0u8; 64];
    let length = unsafe {
        libc::strftime(
            result.as_mut_ptr().cast(),
            result.len(),
            c"%Y-%m-%dT%H:%M:%SZ".as_ptr(),
            tm.as_ptr(),
        )
    };
    ensure!(length > 0, "format snapshot timestamp");
    Ok(String::from_utf8(result[..length].to_vec())?)
}

fn fnv(mut hash: u64, bytes: &[u8]) -> u64 {
    for byte in bytes {
        hash = (hash ^ u64::from(*byte)).wrapping_mul(FNV_PRIME);
    }
    hash
}

fn parse_hash(field: Option<&str>, name: &str) -> Result<u64> {
    let value = field.unwrap_or("");
    ensure!(
        value.len() == 16
            && value
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)),
        "{name} must be 16 lowercase hex digits"
    );
    Ok(u64::from_str_radix(value, 16)?)
}

fn regular(path: &Path) -> Result<File> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)
        .with_context(|| format!("open {}", path.display()))?;
    ensure!(
        file.metadata()?.is_file(),
        "{} is not a regular file",
        path.display()
    );
    Ok(file)
}

fn sized(path: &Path, size: u64) -> Result<File> {
    let file = regular(path)?;
    ensure!(
        file.metadata()?.len() == size,
        "{} size does not match manifest",
        path.display()
    );
    Ok(file)
}

fn locked(path: &Path, exclusive: bool) -> Result<File> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .with_context(|| format!("open disk {}", path.display()))?;
    ensure!(
        file.metadata()?.is_file(),
        "{} is not a regular file",
        path.display()
    );
    let operation = if exclusive {
        libc::LOCK_EX
    } else {
        libc::LOCK_SH
    };
    if unsafe { libc::flock(file.as_raw_fd(), operation | libc::LOCK_NB) } < 0 {
        let error = io::Error::last_os_error();
        if error.raw_os_error() == Some(libc::EWOULDBLOCK) {
            bail!(
                "disk image {} is in use by a running sandbox",
                path.display()
            );
        }
        return Err(error).context("lock disk");
    }
    Ok(file)
}

fn copy_disk(source: &File, output: Option<&File>, length: u64) -> Result<u64> {
    let mut page = vec![0u8; MIB];
    let mut hash = FNV_OFFSET;
    let mut offset = 0;
    while offset < length {
        let size = (length - offset).min(page.len() as u64) as usize;
        let data = &mut page[..size];
        source.read_exact_at(data, offset)?;
        hash = fnv(hash, data);
        if let Some(file) = output {
            if data.iter().any(|&byte| byte != 0) {
                file.write_all_at(data, offset)?;
            }
        }
        offset += size as u64;
    }
    ensure!(
        source.metadata()?.len() == length,
        "disk size changed while copying"
    );
    if let Some(file) = output {
        file.set_len(length)?;
        file.sync_all()?;
    }
    Ok(hash)
}

fn safe_time(value: &str) -> bool {
    value.len() == 20
        && value.bytes().enumerate().all(|(index, ch)| match index {
            4 | 7 => ch == b'-',
            10 => ch == b'T',
            13 | 16 => ch == b':',
            19 => ch == b'Z',
            _ => ch.is_ascii_digit(),
        })
}

impl Manifest {
    fn new(name: &str, kind: &str, info: Info) -> Result<Self> {
        Ok(Self {
            manifest_version: 1,
            name: name.to_owned(),
            snapshot_type: kind.to_owned(),
            created_at: created_at()?,
            kernel_path: info.kernel,
            initrd_path: info.initrd,
            cmdline: info.cmdline,
            guest_mem_mb: info.mem,
            disk_filename: None,
            disk_path: None,
            disk_size_bytes: None,
            disk_fnv1a64: None,
            memory_filename: None,
            memory_size_bytes: None,
            memory_hash: None,
            vmstate_filename: None,
            vmstate_size_bytes: None,
            vmstate_fnv1a64: None,
            disk: None,
            net: None,
        })
    }

    fn add_disk(&mut self, source: &File, disk_path: &Path, destination: &Path) -> Result<()> {
        let length = source.metadata()?.len();
        ensure!(length > 0, "disk_size_bytes must be a positive integer");
        let file = File::create_new(destination.join("disk.img"))?;
        let hash = copy_disk(source, Some(&file), length)?;
        self.disk_filename = Some("disk.img".into());
        let path = disk_path
            .canonicalize()
            .unwrap_or_else(|_| disk_path.to_owned());
        self.disk_path = Some(path.to_string_lossy().into_owned());
        self.disk_size_bytes = Some(length);
        self.disk_fnv1a64 = Some(format!("{hash:016x}"));
        Ok(())
    }

    fn load(name: &str) -> Result<(PathBuf, Self)> {
        let path = directory(name)?;
        ensure!(
            path.symlink_metadata()?.is_dir(),
            "snapshot '{name}' is not a directory"
        );
        let metadata = regular(&path.join("metadata.json"))?;
        ensure!(
            metadata.metadata()?.len() <= 65536,
            "metadata.json is too large"
        );
        let manifest: Self =
            serde_json::from_reader(metadata.take(65537)).context("invalid snapshot manifest")?;
        ensure!(
            manifest.manifest_version == 1,
            "unsupported manifest_version {}",
            manifest.manifest_version
        );
        ensure!(
            manifest.name == name,
            "name in manifest does not match the directory name"
        );
        ensure!(
            safe_time(&manifest.created_at),
            "created_at missing or malformed"
        );
        match manifest.snapshot_type.as_str() {
            "warm" => {
                let mem = manifest
                    .guest_mem_mb
                    .filter(|mem| (64..=3072).contains(mem))
                    .context("guest_mem_mb must be 64–3072")?;
                ensure!(
                    manifest.memory_filename.as_deref() == Some("memory.bin"),
                    "memory_filename must be memory.bin"
                );
                ensure!(
                    manifest.memory_size_bytes == Some((mem * MIB) as u64),
                    "memory_size_bytes must equal guest_mem_mb MiB"
                );
                parse_hash(manifest.memory_hash.as_deref(), "memory_hash")?;
                sized(&path.join("memory.bin"), (mem * MIB) as u64)?;
                ensure!(
                    manifest.vmstate_filename.as_deref() == Some("vmstate.bin"),
                    "vmstate_filename must be vmstate.bin"
                );
                let size = manifest
                    .vmstate_size_bytes
                    .filter(|size| (1..=MIB as u64).contains(size))
                    .context("invalid vmstate_size_bytes")?;
                parse_hash(manifest.vmstate_fnv1a64.as_deref(), "vmstate_fnv1a64")?;
                sized(&path.join("vmstate.bin"), size)?;
                ensure!(
                    manifest.disk.is_some_and(|n| n <= 1) && manifest.net.is_some_and(|n| n <= 1),
                    "disk and net must be 0 or 1"
                );
                if manifest.disk == Some(0) {
                    ensure!(
                        absent(&path.join("disk.img"))?,
                        "manifest says disk 0 but disk.img exists"
                    );
                }
            }
            "cold" => {
                for file in ["memory.bin", "vmstate.bin"] {
                    ensure!(
                        absent(&path.join(file))?,
                        "{file} is unexpected in a cold snapshot"
                    );
                }
            }
            _ => bail!("snapshot_type must be cold or warm"),
        }
        if manifest.has_disk() {
            ensure!(
                manifest.disk_filename.as_deref() == Some("disk.img"),
                "disk_filename must be disk.img"
            );
            let size = manifest
                .disk_size_bytes
                .filter(|n| *n > 0)
                .context("disk_size_bytes must be a positive integer")?;
            sized(&path.join("disk.img"), size)?;
            parse_hash(manifest.disk_fnv1a64.as_deref(), "disk_fnv1a64")?;
        }
        Ok((path, manifest))
    }

    fn has_disk(&self) -> bool {
        self.snapshot_type == "cold" || self.disk == Some(1)
    }

    fn verify_disk(&self, path: &Path, output: Option<&File>) -> Result<()> {
        let size = self.disk_size_bytes.context("no saved disk")?;
        let file = sized(&path.join("disk.img"), size)?;
        let hash = copy_disk(&file, output, size)?;
        ensure!(
            hash == parse_hash(self.disk_fnv1a64.as_deref(), "disk_fnv1a64")?,
            "disk.img content does not match disk_fnv1a64"
        );
        Ok(())
    }

    fn read_state(&self, path: &Path) -> Result<Vec<u8>> {
        let length = self
            .vmstate_size_bytes
            .context("missing vmstate_size_bytes")?;
        let mut data = Vec::new();
        sized(&path.join("vmstate.bin"), length)?
            .take(length + 1)
            .read_to_end(&mut data)?;
        ensure!(
            data.len() as u64 == length
                && fnv(FNV_OFFSET, &data)
                    == parse_hash(self.vmstate_fnv1a64.as_deref(), "vmstate_fnv1a64")?,
            "vmstate.bin content does not match vmstate_fnv1a64"
        );
        Ok(data)
    }

    fn prepare_disk(&self, directory: &Path, disk: &Path) -> Result<PendingDisk> {
        let (target, _lock, mode) = match disk.canonicalize() {
            Ok(path) => {
                let file = locked(&path, true)?;
                let mode = file.metadata()?.permissions().mode();
                (path, Some(file), mode)
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound && absent(disk)? => {
                (disk.to_owned(), None, 0o644)
            }
            Err(error) => return Err(error).context("resolve disk destination"),
        };
        let parent = target
            .parent()
            .filter(|path| !path.as_os_str().is_empty())
            .unwrap_or(Path::new("."));
        let temp = tempfile::Builder::new()
            .prefix(".svmm-restore-")
            .tempfile_in(parent)?;
        temp.as_file()
            .set_permissions(fs::Permissions::from_mode(mode))?;
        self.verify_disk(directory, Some(temp.as_file()))?;
        Ok(PendingDisk {
            target,
            temp,
            _lock,
            name: self.name.clone(),
            size: self.disk_size_bytes.unwrap_or(0),
            created: self.created_at.clone(),
        })
    }
}

pub struct PendingDisk {
    target: PathBuf,
    temp: tempfile::NamedTempFile,
    _lock: Option<File>,
    name: String,
    size: u64,
    created: String,
}

impl PendingDisk {
    pub fn path(&self) -> &Path {
        self.temp.path()
    }
    pub fn publish(self) -> Result<()> {
        let parent = self
            .target
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new("."));
        self.temp
            .persist(&self.target)
            .with_context(|| format!("replace {}", self.target.display()))?;
        File::open(parent)?.sync_all()?;
        eprintln!(
            "INFO: snapshot: workspace restored from snapshot '{}' ({} bytes, created {})",
            self.name, self.size, self.created
        );
        Ok(())
    }
}

fn absent(path: &Path) -> Result<bool> {
    match path.symlink_metadata() {
        Ok(_) => Ok(false),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(true),
        Err(error) => Err(error.into()),
    }
}

pub fn check_new(name: &str) -> Result<()> {
    ensure!(
        absent(&directory(name)?)?,
        "snapshot '{name}' already exists"
    );
    Ok(())
}

fn publish(name: &str, write: impl FnOnce(&Path) -> Result<Manifest>) -> Result<()> {
    check_new(name)?;
    let destination = directory(name)?;
    let root = root();
    fs::create_dir_all(&root)?;
    let temp = tempfile::Builder::new().prefix(".tmp-").tempdir_in(&root)?;
    let manifest = write(temp.path())?;
    let mut file = File::create_new(temp.path().join("metadata.json"))?;
    serde_json::to_writer_pretty(&mut file, &manifest)?;
    file.write_all(b"\n")?;
    file.sync_all()?;
    File::open(temp.path())?.sync_all()?;
    let from = CString::new(temp.path().as_os_str().as_bytes())?;
    let to = CString::new(destination.as_os_str().as_bytes())?;
    // Refuse to overwrite any concurrently published snapshot.
    let result = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            from.as_ptr(),
            libc::AT_FDCWD,
            to.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    if result != 0 {
        return Err(io::Error::last_os_error()).context("publish snapshot");
    }
    File::open(root)?.sync_all()?;
    Ok(())
}

pub fn save(name: &str, disk: &Path, info: Info) -> Result<()> {
    let file = locked(disk, false)?;
    publish(name, |path| {
        let mut manifest = Manifest::new(name, "cold", info)?;
        manifest.add_disk(&file, disk, path)?;
        Ok(manifest)
    })?;
    eprintln!("INFO: snapshot '{name}' saved successfully");
    Ok(())
}

pub fn restore_cold(name: &str, disk: &Path) -> Result<()> {
    eprintln!(
        "INFO: snapshot: restoring workspace from cold snapshot '{name}' into {}",
        disk.display()
    );
    let (path, manifest) =
        Manifest::load(name).context("manifest validation failed, restore aborted")?;
    ensure!(
        manifest.snapshot_type == "cold",
        "cannot restore a warm snapshot with --snapshot"
    );
    manifest.prepare_disk(&path, disk)?.publish()
}

pub fn list() -> Result<()> {
    let entries = match fs::read_dir(root()) {
        Ok(entries) => entries,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error.into()),
    };
    let mut names = Vec::new();
    for entry in entries {
        let entry = entry?;
        if entry.file_type()?.is_dir() {
            if let Ok(name) = entry.file_name().into_string() {
                if crate::cli::identifier(&name, 128, false) {
                    names.push(name);
                }
            }
        }
    }
    names.sort();
    for name in names {
        match Manifest::load(&name) {
            Ok((_, m)) => println!(
                "{name}\t{}\t{}\t{}",
                m.disk_size_bytes.unwrap_or(0),
                m.created_at,
                m.snapshot_type
            ),
            Err(_) => println!("{name}\t-\tinvalid"),
        }
    }
    Ok(())
}

pub fn delete(name: &str) -> Result<()> {
    let path = directory(name)?;
    ensure!(
        path.symlink_metadata()?.is_dir(),
        "snapshot is not a directory"
    );
    let mut files = Vec::new();
    for entry in fs::read_dir(&path)? {
        let entry = entry?;
        let known = entry.file_name().to_str().is_some_and(|name| {
            matches!(
                name,
                "metadata.json" | "disk.img" | "memory.bin" | "vmstate.bin"
            )
        });
        ensure!(
            known && entry.file_type()?.is_file(),
            "snapshot has unexpected file {}",
            entry.path().display()
        );
        files.push(entry.path());
    }
    for file in files {
        fs::remove_file(file)?;
    }
    fs::remove_dir(path)?;
    File::open(root())?.sync_all()?;
    eprintln!("INFO: snapshot '{name}' deleted");
    Ok(())
}

struct MemoryWriter<'a> {
    file: Option<&'a File>,
    page: [u8; PAGE],
    used: usize,
    offset: u64,
    hash: u64,
}

impl<'a> MemoryWriter<'a> {
    fn new(file: Option<&'a File>) -> Self {
        Self {
            file,
            page: [0; PAGE],
            used: 0,
            offset: 0,
            hash: FNV_OFFSET,
        }
    }
}

impl Write for MemoryWriter<'_> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let mut rest = bytes;
        while !rest.is_empty() {
            let limit = rest.len().min(PAGE - self.used);
            let (head, tail) = rest.split_at(limit);
            self.page[self.used..self.used + limit].copy_from_slice(head);
            rest = tail;
            self.used += limit;
            if self.used == PAGE {
                if self.page.iter().any(|&byte| byte != 0) {
                    self.hash = (self.hash ^ (self.offset / PAGE as u64)).wrapping_mul(FNV_PRIME);
                    for word in self.page.chunks_exact(8) {
                        self.hash = (self.hash ^ u64::from_le_bytes(word.try_into().unwrap()))
                            .wrapping_mul(FNV_PRIME);
                    }
                    if let Some(file) = self.file {
                        file.write_all_at(&self.page, self.offset)?;
                    }
                }
                self.offset += PAGE as u64;
                self.used = 0;
            }
        }
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

pub fn save_warm(
    name: &str,
    info: Info,
    mem: &crate::memory::Memory,
    has_net: bool,
    state: &[u8],
    disk: Option<(&File, &Path)>,
) -> Result<()> {
    publish(name, |path| {
        let mut manifest = Manifest::new(name, "warm", info)?;
        let size = manifest.guest_mem_mb.context("missing guest memory size")? * MIB;
        let file = File::create_new(path.join("memory.bin"))?;
        let mut writer = MemoryWriter::new(Some(&file));
        mem.dump(&mut writer)?;
        ensure!(
            writer.offset == size as u64 && writer.used == 0,
            "snapshot memory size changed"
        );
        file.set_len(size as u64)?;
        file.sync_all()?;
        let mut state_file = File::create_new(path.join("vmstate.bin"))?;
        state_file.write_all(state)?;
        state_file.sync_all()?;
        manifest.memory_filename = Some("memory.bin".into());
        manifest.memory_size_bytes = Some(size as u64);
        manifest.memory_hash = Some(format!("{:016x}", writer.hash));
        manifest.vmstate_filename = Some("vmstate.bin".into());
        manifest.vmstate_size_bytes = Some(state.len() as u64);
        manifest.vmstate_fnv1a64 = Some(format!("{:016x}", fnv(FNV_OFFSET, state)));
        manifest.disk = Some(u8::from(disk.is_some()));
        manifest.net = Some(u8::from(has_net));
        if let Some((file, disk_path)) = disk {
            manifest.add_disk(file, disk_path, path)?;
        }
        Ok(manifest)
    })?;
    eprintln!("INFO: warm snapshot '{name}' saved");
    Ok(())
}

pub struct Warm {
    path: PathBuf,
    manifest: Manifest,
    pub mem_mib: usize,
    pub memory: File,
    pub state: Vec<u8>,
}

impl Warm {
    pub fn open(name: &str, disk: bool, net: bool) -> Result<Self> {
        let (path, manifest) = Manifest::load(name)?;
        ensure!(
            manifest.snapshot_type == "warm",
            "cannot --restore a cold snapshot"
        );
        ensure!(
            manifest.has_disk() == disk,
            "snapshot virtio-blk device does not match --disk"
        );
        ensure!(
            (manifest.net == Some(1)) == net,
            "snapshot virtio-net device does not match --tap"
        );
        let memory = sized(
            &path.join("memory.bin"),
            manifest.memory_size_bytes.unwrap(),
        )?;
        let state = manifest.read_state(&path)?;
        Ok(Self {
            mem_mib: manifest.guest_mem_mb.unwrap(),
            memory,
            path,
            manifest,
            state,
        })
    }

    pub fn verify_mapping(&self, memory: &crate::memory::Memory) -> Result<()> {
        let mut writer = MemoryWriter::new(None);
        memory.dump(&mut writer)?;
        self.verify_memory_hash(writer.hash)
    }

    fn verify_memory_hash(&self, hash: u64) -> Result<()> {
        ensure!(
            hash == parse_hash(self.manifest.memory_hash.as_deref(), "memory_hash")?,
            "memory.bin content does not match memory_hash"
        );
        Ok(())
    }

    pub fn prepare_disk(&self, disk: &Path) -> Result<PendingDisk> {
        self.manifest.prepare_disk(&self.path, disk)
    }
}

pub fn validate(name: &str) -> Result<()> {
    let (path, manifest) = Manifest::load(name)?;
    if manifest.has_disk() {
        manifest.verify_disk(&path, None)?;
    }
    if manifest.snapshot_type == "warm" {
        let warm = Warm::open(name, manifest.has_disk(), manifest.net == Some(1))?;
        crate::vmstate::State::decode(&warm.state, manifest.has_disk(), manifest.net == Some(1))?;
        let mut memory = &warm.memory;
        memory.seek(SeekFrom::Start(0))?;
        let mut writer = MemoryWriter::new(None);
        io::copy(&mut memory, &mut writer)?;
        warm.verify_memory_hash(writer.hash)?;
    }
    eprintln!("INFO: snapshot '{name}': manifest valid");
    Ok(())
}

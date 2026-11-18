//! OCI image export and workload boot asset selection.
use std::collections::HashMap;
use std::env;
use std::ffi::{CStr, CString, OsStr, OsString};
use std::fs::{self, DirBuilder, File};
use std::io::{self, Read, Seek, SeekFrom};
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, FromRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
use std::os::unix::process::CommandExt;
use std::path::{Component, Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail, ensure};
use serde::Deserialize;
use serde_json::Value;
use tar::{Archive, Entry, EntryType};
use tempfile::{Builder as TempBuilder, TempDir, tempfile};

use crate::cancelled;
use crate::workload_config::{CONFIG_HEADER_SIZE, CONFIG_MAGIC, CONFIG_MAX, ITEMS_MAX};

const MIB: u64 = 1024 * 1024;
const CREATE_CANCEL_GRACE: Duration = Duration::from_secs(1);
const CLEANUP_CANCEL_GRACE: Duration = Duration::from_secs(3);

pub struct ImageOptions {
    pub image: String,
    pub entrypoint: Option<String>,
    pub env: Vec<String>,
    pub user: Option<String>,
    pub workdir: Option<String>,
    pub size_mb: Option<u64>,
}

pub struct PreparedImage {
    pub disk: PathBuf,
    _directory: TempDir,
}

#[derive(Deserialize)]
#[serde(rename_all = "PascalCase")]
struct ImageConfig {
    entrypoint: Option<Vec<String>>,
    cmd: Option<Vec<String>>,
    env: Option<Vec<String>>,
    working_dir: Option<String>,
    user: Option<String>,
}

fn encode_config(
    config: &ImageConfig,
    options: &ImageOptions,
    command: &[String],
) -> Result<Vec<u8>> {
    let entrypoint = match options.entrypoint.as_deref() {
        Some("") => &[],
        Some(_) => options.entrypoint.as_slice(),
        None => config.entrypoint.as_deref().unwrap_or_default(),
    };
    let command = if command.is_empty() && options.entrypoint.is_none() {
        config.cmd.as_deref().unwrap_or_default()
    } else {
        command
    };
    let argv: Vec<_> = entrypoint.iter().chain(command).collect();
    ensure!(
        argv.first()
            .is_some_and(|executable| !executable.is_empty()),
        "image has no executable; supply --entrypoint or a command"
    );

    let mut env_entries: Vec<&str> = Vec::new();
    let mut indices = HashMap::new();
    for entry in config
        .env
        .as_deref()
        .unwrap_or_default()
        .iter()
        .chain(&options.env)
        .map(String::as_str)
    {
        let (key, _) = entry
            .split_once('=')
            .filter(|(key, _)| !key.is_empty())
            .context("environment must be KEY=VALUE")?;
        match indices.entry(key) {
            std::collections::hash_map::Entry::Occupied(index) => env_entries[*index.get()] = entry,
            std::collections::hash_map::Entry::Vacant(index) => {
                index.insert(env_entries.len());
                env_entries.push(entry);
            }
        }
    }

    let cwd = options.workdir.as_deref().unwrap_or_else(|| {
        config
            .working_dir
            .as_deref()
            .filter(|cwd| !cwd.is_empty())
            .unwrap_or("/")
    });
    let user = options.user.as_deref().unwrap_or_else(|| {
        config
            .user
            .as_deref()
            .filter(|user| !user.is_empty())
            .unwrap_or("0:0")
    });
    ensure!(cwd.starts_with('/'), "working directory must be absolute");
    ensure!(
        argv.len() <= ITEMS_MAX && env_entries.len() <= ITEMS_MAX,
        "too many command arguments or environment variables"
    );
    let strings = [cwd, user]
        .into_iter()
        .chain(argv.iter().map(|arg| arg.as_str()))
        .chain(env_entries.iter().copied());
    let mut size = CONFIG_HEADER_SIZE;
    for value in strings.clone() {
        ensure!(
            !value.as_bytes().contains(&0),
            "configuration strings must not contain NUL"
        );
        size += value.len() + 1;
        ensure!(size <= CONFIG_MAX, "workload configuration exceeds 1 MiB");
    }
    let mut config = Vec::with_capacity(size);
    config.extend(CONFIG_MAGIC);
    config.extend((argv.len() as u32).to_le_bytes());
    config.extend((env_entries.len() as u32).to_le_bytes());
    for value in strings {
        config.extend(value.as_bytes());
        config.push(0);
    }
    Ok(config)
}

fn syscall(result: i32) -> io::Result<i32> {
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(result)
    }
}

fn archive_components(path: &Path) -> Result<Vec<CString>> {
    path.components()
        .filter_map(|component| match component {
            Component::Normal(name) => Some(CString::new(name.as_bytes()).map_err(Into::into)),
            Component::CurDir => None,
            _ => Some(Err(anyhow::anyhow!("unsafe archive path: {path:?}"))),
        })
        .collect()
}

fn openat(directory: &OwnedFd, name: &CStr, flags: i32, mode: libc::mode_t) -> io::Result<OwnedFd> {
    let fd = syscall(unsafe {
        libc::openat(
            directory.as_raw_fd(),
            name.as_ptr(),
            flags | libc::O_CLOEXEC | libc::O_NOFOLLOW,
            mode,
        )
    })?;
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

fn directory(root: &OwnedFd, components: &[CString]) -> Result<OwnedFd> {
    let mut fd = root.try_clone()?;
    for component in components {
        match syscall(unsafe { libc::mkdirat(fd.as_raw_fd(), component.as_ptr(), 0o700) }) {
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
            Err(error) => return Err(error).with_context(|| format!("mkdir {component:?}")),
        }
        fd = openat(&fd, component, libc::O_RDONLY | libc::O_DIRECTORY, 0)
            .with_context(|| format!("open directory {component:?}"))?;
    }
    Ok(fd)
}

struct Attributes {
    uid: u32,
    gid: u32,
    mode: libc::mode_t,
    modified: libc::timespec,
}

fn timestamp(entry: &mut Entry<'_, File>) -> Result<libc::timespec> {
    if let Some(extensions) = entry.pax_extensions()? {
        for extension in extensions {
            let extension = extension?;
            if extension.key_bytes() == b"mtime" {
                let value = extension.value()?;
                let (seconds, fraction) = value.split_once('.').unwrap_or((value, ""));
                let seconds: i64 = seconds.parse().context("invalid archive timestamp")?;
                ensure!(
                    fraction.bytes().all(|digit| digit.is_ascii_digit()),
                    "invalid archive timestamp"
                );
                let mut nanos = 0;
                for digit in fraction.bytes().take(9) {
                    nanos = nanos * 10 + i64::from(digit - b'0');
                }
                nanos *= 10_i64.pow(9 - fraction.len().min(9) as u32);
                let (seconds, nanos) = if value.starts_with('-') && nanos != 0 {
                    (
                        seconds
                            .checked_sub(1)
                            .context("archive timestamp underflow")?,
                        1_000_000_000 - nanos,
                    )
                } else {
                    (seconds, nanos)
                };
                return Ok(libc::timespec {
                    tv_sec: seconds,
                    tv_nsec: nanos,
                });
            }
        }
    }
    Ok(libc::timespec {
        tv_sec: entry.header().mtime()?.try_into()?,
        tv_nsec: 0,
    })
}

impl Attributes {
    fn read(entry: &mut Entry<'_, File>) -> Result<Self> {
        Ok(Self {
            uid: entry.header().uid()?.try_into()?,
            gid: entry.header().gid()?.try_into()?,
            mode: entry.header().mode()? & 0o7777,
            modified: timestamp(entry)?,
        })
    }

    fn apply(&self, object: BorrowedFd<'_>) -> Result<()> {
        let fd = object.as_raw_fd();
        syscall(unsafe { libc::fchown(fd, self.uid, self.gid) }).context("set archive owner")?;
        syscall(unsafe { libc::fchmod(fd, self.mode) }).context("set archive permissions")?;
        let times = [self.modified; 2];
        syscall(unsafe { libc::futimens(fd, times.as_ptr()) }).context("set archive timestamp")?;
        Ok(())
    }
}

struct HardLink {
    path: Vec<CString>,
    source: Vec<CString>,
}

impl HardLink {
    fn install(&self, root: &OwnedFd) -> Result<bool> {
        let (source_name, source_parent) =
            self.source.split_last().context("empty hardlink target")?;
        let (dest_name, dest_parent) = self.path.split_last().context("empty hardlink path")?;
        let source = directory(root, source_parent)?;
        let dest = directory(root, dest_parent)?;
        match syscall(unsafe {
            libc::linkat(
                source.as_raw_fd(),
                source_name.as_ptr(),
                dest.as_raw_fd(),
                dest_name.as_ptr(),
                0,
            )
        }) {
            Ok(_) => Ok(true),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
            Err(error) => Err(error).context("extract archive hardlink"),
        }
    }
}

fn extract(archive_path: &Path, root: &Path) -> Result<u64> {
    let root = OwnedFd::from(File::open(root)?);
    let mut archive = Archive::new(File::open(archive_path)?);
    let mut directories = Vec::new();
    let mut links = Vec::new();
    let mut used = 0_u64;
    for entry in archive.entries()? {
        ensure_running()?;
        let mut entry = entry?;
        let path = archive_components(&entry.path()?)?;
        let Some((name, components)) = path.split_last() else {
            continue;
        };
        let parent = directory(&root, components)?;
        let fd = parent.as_raw_fd();
        match entry.header().entry_type() {
            EntryType::Directory => {
                directory(&root, &path)?;
                directories.push((path, Attributes::read(&mut entry)?));
            }
            EntryType::Regular | EntryType::Continuous | EntryType::GNUSparse => {
                let attributes = Attributes::read(&mut entry)?;
                let mut object = File::from(openat(
                    &parent,
                    name,
                    libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL,
                    0o600,
                )?);
                let size = entry.size();
                let mut contents = entry.take(size);
                while contents.limit() > 0 {
                    ensure_running()?;
                    let count = io::copy(&mut (&mut contents).take(MIB), &mut object)?;
                    ensure!(count > 0, "truncated archive file {name:?}");
                }
                attributes.apply(object.as_fd())?;
                used = used.checked_add(size).context("archive size overflow")?;
            }
            EntryType::Symlink => {
                let target = entry.link_name()?.context("missing symlink target")?;
                let target = CString::new(target.as_os_str().as_bytes())?;
                let uid = entry.header().uid()?.try_into()?;
                let gid = entry.header().gid()?.try_into()?;
                syscall(unsafe { libc::symlinkat(target.as_ptr(), fd, name.as_ptr()) })?;
                syscall(unsafe {
                    libc::fchownat(fd, name.as_ptr(), uid, gid, libc::AT_SYMLINK_NOFOLLOW)
                })
                .context("set symlink owner")?;
            }
            EntryType::Link => {
                let source =
                    archive_components(&entry.link_name()?.context("missing hardlink target")?)?;
                links.push(HardLink { path, source });
            }
            EntryType::Fifo => {
                let attributes = Attributes::read(&mut entry)?;
                syscall(unsafe { libc::mkfifoat(fd, name.as_ptr(), attributes.mode) })?;
                let object = openat(&parent, name, libc::O_RDONLY | libc::O_NONBLOCK, 0)?;
                attributes.apply(object.as_fd())?;
            }
            _ => bail!("unsupported special file in image: {:?}", entry.path()?),
        }
    }

    // Docker archives can refer to a hardlink target appearing later in the stream.
    while !links.is_empty() {
        let mut pending = Vec::new();
        let count = links.len();
        for link in links {
            ensure_running()?;
            if !link.install(&root)? {
                pending.push(link);
            }
        }
        ensure!(pending.len() < count, "unresolved archive hardlinks");
        links = pending;
    }
    for (path, attributes) in directories.into_iter().rev() {
        ensure_running()?;
        attributes.apply(directory(&root, &path)?.as_fd())?;
    }
    Ok(used)
}

fn ensure_running() -> Result<()> {
    ensure!(cancelled().is_none(), "image preparation cancelled");
    Ok(())
}

fn signal_child_group(child: &Child, signal: libc::c_int) {
    unsafe { libc::kill(-(child.id() as i32), signal) };
}

fn wait_child(
    child: &mut Child,
    grace: Duration,
    deadline: &mut Option<Instant>,
) -> io::Result<Option<ExitStatus>> {
    loop {
        if let Some(status) = child.try_wait()? {
            return Ok(Some(status));
        }
        if cancelled().is_some() {
            let deadline = deadline.get_or_insert_with(|| {
                if !grace.is_zero() {
                    signal_child_group(child, libc::SIGCONT);
                }
                Instant::now() + grace
            });
            if Instant::now() >= *deadline {
                signal_child_group(child, libc::SIGKILL);
                child.wait()?;
                return Ok(None);
            }
        }
        thread::sleep(Duration::from_millis(10));
    }
}

fn run_child(command: &mut Command, cancellation_grace: Duration) -> Result<ExitStatus> {
    ensure_running()?;
    let mut child = command
        .process_group(0)
        .spawn()
        .with_context(|| format!("start {command:?}"))?;
    let status = wait_child(&mut child, cancellation_grace, &mut None)?
        .context("image preparation cancelled")?;
    ensure_running()?;
    Ok(status)
}

fn checked_child(command: &mut Command, diagnostic_stdout: bool) -> Result<()> {
    if diagnostic_stdout {
        command.stdout(Stdio::from(io::stderr().as_fd().try_clone_to_owned()?));
    }
    let status = run_child(command, Duration::ZERO)?;
    ensure!(status.success(), "{command:?} failed with {status}");
    Ok(())
}

fn capture(
    command: &mut Command,
    cancellation_grace: Duration,
) -> Result<(ExitStatus, Vec<u8>, Vec<u8>)> {
    // Files allow waiting for cancellation without stalling on a full subprocess pipe.
    let mut stdout = tempfile()?;
    let mut stderr = tempfile()?;
    command
        .stdout(stdout.try_clone()?)
        .stderr(stderr.try_clone()?);
    let status = run_child(command, cancellation_grace)?;
    fn read_back(file: &mut File) -> io::Result<Vec<u8>> {
        let mut output = Vec::new();
        file.seek(SeekFrom::Start(0))?;
        file.read_to_end(&mut output)?;
        Ok(output)
    }
    Ok((status, read_back(&mut stdout)?, read_back(&mut stderr)?))
}

fn inspect_image(image: &str, diagnostic_stdout: bool) -> Result<Value> {
    let inspect = || {
        capture(
            Command::new("docker").args(["image", "inspect", "--", image]),
            Duration::ZERO,
        )
    };
    let (mut status, mut output, mut error) = inspect()?;
    if !status.success() {
        checked_child(
            Command::new("docker").args(["pull", "--platform", "linux/amd64", "--", image]),
            diagnostic_stdout,
        )?;
        (status, output, error) = inspect()?;
    }
    ensure!(
        status.success(),
        "docker image inspect {image:?} failed with {status}: {}",
        String::from_utf8_lossy(&error).trim_end()
    );
    let inspected: Vec<Value> =
        serde_json::from_slice(&output).context("read Docker image metadata")?;
    inspected
        .into_iter()
        .next()
        .context("Docker inspect returned no image")
}

struct ExportContainer {
    name: String,
    retry_cleanup: bool,
}

impl Drop for ExportContainer {
    fn drop(&mut self) {
        let mut deadline = cancelled().map(|_| Instant::now() + CLEANUP_CANCEL_GRACE);
        loop {
            let Ok(mut child) = Command::new("docker")
                .args(["rm", "-f", "-v", &self.name])
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .process_group(0)
                .spawn()
            else {
                return;
            };
            let Ok(Some(status)) = wait_child(&mut child, CLEANUP_CANCEL_GRACE, &mut deadline)
            else {
                return;
            };
            if status.success()
                || !self.retry_cleanup
                || deadline.is_none_or(|deadline| Instant::now() >= deadline)
            {
                return;
            }
            // The daemon can finish a create after the Docker client is gone.
            thread::sleep(Duration::from_millis(50));
        }
    }
}

fn export_image(id: &str, work: &TempDir, root: &Path, diagnostic_stdout: bool) -> Result<u64> {
    let name = format!(
        "svmm-workload-{}",
        work.path()
            .file_name()
            .context("missing workspace name")?
            .to_string_lossy()
    );
    let mut container = ExportContainer {
        name,
        retry_cleanup: false,
    };
    let created = capture(
        Command::new("docker").args([
            "create",
            "--name",
            &container.name,
            "--label",
            "svmm.workload-builder=true",
            "--entrypoint",
            "/__svmm_export_only__",
            id,
        ]),
        // Give a cancelled create time to finish before removing the known name.
        CREATE_CANCEL_GRACE,
    );
    container.retry_cleanup = created.is_err() && cancelled().is_some();
    let (status, _, stderr) = created?;
    ensure!(
        status.success(),
        "docker create failed with {status}: {}",
        String::from_utf8_lossy(&stderr).trim_end()
    );
    let archive = work.path().join("rootfs.tar");
    checked_child(
        Command::new("docker")
            .args(["export", "--output"])
            .arg(&archive)
            .arg(&container.name),
        diagnostic_stdout,
    )?;
    extract(&archive, root)
}

fn accessible(path: &Path, mode: i32) -> bool {
    CString::new(path.as_os_str().as_bytes())
        .is_ok_and(|path| unsafe { libc::access(path.as_ptr(), mode) == 0 })
}

fn exists(path: &Path) -> Result<bool> {
    match fs::symlink_metadata(path) {
        Ok(_) => Ok(true),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error).with_context(|| format!("check {}", path.display())),
    }
}

fn check_tools() -> Result<()> {
    let path = env::var_os("PATH").unwrap_or_else(|| OsString::from("/bin:/usr/bin"));
    for tool in ["docker", "mkfs.ext4"] {
        ensure!(
            env::split_paths(&path).any(|path| {
                let executable = path.join(tool);
                executable.is_file() && accessible(&executable, libc::X_OK)
            }),
            "missing required tool: {tool}"
        );
    }
    Ok(())
}

fn build_disk(
    options: &ImageOptions,
    command: &[String],
    output: &Path,
    diagnostic_stdout: bool,
) -> Result<()> {
    ensure_running()?;
    let output = std::path::absolute(output)?;
    ensure!(
        !exists(&output)?,
        "output already exists: {}",
        output.display()
    );
    ensure!(
        unsafe { libc::geteuid() } == 0,
        "run the builder as root to preserve image UID/GID and permissions"
    );
    check_tools()?;
    ensure!(
        options.size_mb.is_none_or(|size| size >= 16),
        "--size-mb must be at least 16"
    );
    let image = inspect_image(&options.image, diagnostic_stdout)?;
    ensure!(
        image["Os"] == "linux" && image["Architecture"] == "amd64",
        "only linux/amd64 images are supported"
    );
    let id = image["Id"].as_str().context("missing Docker image ID")?;
    let config =
        serde_json::from_value(image["Config"].clone()).context("read Docker image config")?;
    let blob = encode_config(&config, options, command)?;

    let parent = output.parent().context("output has no parent directory")?;
    fs::create_dir_all(parent)?;
    let work = TempBuilder::new()
        .prefix("svmm-workload-build-")
        .rand_bytes(24)
        .tempdir()?;
    let root = work.path().join("root");
    fs::create_dir(&root)?;
    let used = export_image(id, &work, &root, diagnostic_stdout)?;

    // A reserved path from the archive must never redirect metadata writes.
    let metadata = root.join(".svmm");
    ensure!(!exists(&metadata)?, "image contains reserved /.svmm path");
    DirBuilder::new().mode(0o700).create(&metadata)?;
    let mut json = serde_json::to_vec_pretty(&image)?;
    json.push(b'\n');
    for (name, contents) in [("workload.bin", blob), ("image.json", json)] {
        let file = metadata.join(name);
        fs::write(&file, contents)?;
        fs::set_permissions(file, fs::Permissions::from_mode(0o600))?;
    }

    let size_mb = options
        .size_mb
        .unwrap_or_else(|| 64 + (u128::from(used) * 13).div_ceil(u128::from(10 * MIB)) as u64);
    let size_bytes = size_mb.checked_mul(MIB).context("disk size overflow")?;
    let mut prefix = OsString::from(".");
    prefix.push(output.file_name().context("output has no filename")?);
    prefix.push("-");
    let temporary = TempBuilder::new().prefix(&prefix).tempfile_in(parent)?;
    temporary.as_file().set_len(size_bytes)?;
    checked_child(
        Command::new("mkfs.ext4")
            .args(["-q", "-F", "-d"])
            .arg(&root)
            .arg(temporary.path()),
        diagnostic_stdout,
    )?;
    temporary.as_file().sync_all()?;
    ensure_running()?;
    // Publish atomically, including when a concurrent builder produces the same path.
    temporary
        .persist_noclobber(&output)
        .with_context(|| format!("publish {}", output.display()))?;
    File::open(parent)?.sync_all()?;

    let message = format!("workload: wrote {} ({size_mb} MiB, {id})", output.display());
    if diagnostic_stdout {
        eprintln!("{message}");
    } else {
        println!("{message}");
    }
    Ok(())
}

pub fn build(options: &ImageOptions, command: &[String], output: &Path) -> Result<()> {
    build_disk(options, command, output, false)
}

pub fn prepare(options: &ImageOptions, command: &[String]) -> Result<PreparedImage> {
    let directory = TempBuilder::new().prefix("svmm-image-").tempdir()?;
    let disk = std::path::absolute(directory.path())?.join("rootfs.ext4");
    build_disk(options, command, &disk, true)?;
    Ok(PreparedImage {
        _directory: directory,
        disk,
    })
}

const REQUIRED_KERNEL_OPTIONS: &[&str] = &[
    "CONFIG_ACPI=y",
    "CONFIG_BINFMT_ELF=y",
    "CONFIG_DEVTMPFS=y",
    "CONFIG_VIRTIO_MMIO=y",
    "CONFIG_VIRTIO_BLK=y",
    "CONFIG_EXT4_FS=y",
    "CONFIG_SERIAL_8250_CONSOLE=y",
    "CONFIG_X86_IOPL_IOPERM=y",
    "CONFIG_NET=y",
    "CONFIG_INET=y",
    "CONFIG_VIRTIO_NET=y",
    "CONFIG_BLK_DEV_INITRD=y",
    "CONFIG_RD_GZIP=y",
    "CONFIG_PROC_FS=y",
    "CONFIG_SYSFS=y",
    "CONFIG_TMPFS=y",
    "CONFIG_UNIX98_PTYS=y",
];

fn readable_file(path: &Path) -> bool {
    path.is_file() && accessible(path, libc::R_OK)
}

fn tdx_bzimage(path: &Path) -> io::Result<bool> {
    let mut kernel = File::open(path)?;
    let mut header = [0; 0x264];
    kernel.read_exact(&mut header)?;
    let length = kernel.metadata()?.len();
    let boot_protocol = u16::from_le_bytes([header[0x206], header[0x207]]);
    if header[0x1fe..0x200] != [0x55, 0xaa]
        || &header[0x202..0x206] != b"HdrS"
        || boot_protocol < 0x20c
        || header[0x211] & 1 == 0
        || header[0x236] & 1 == 0
    {
        return Ok(false);
    }
    let setup_sectors = if header[0x1f1] == 0 { 4 } else { header[0x1f1] };
    let setup_size = (u64::from(setup_sectors) + 1) * 512;
    let header_u32 = |offset| {
        let mut value = [0; 4];
        value.copy_from_slice(&header[offset..offset + 4]);
        u64::from(u32::from_le_bytes(value))
    };
    let protected_size = length.saturating_sub(setup_size);
    let payload_size = header_u32(0x1f4) * 16;
    let init_size = header_u32(0x260);
    Ok(setup_size <= 0x10000
        && length <= 512 * MIB
        && payload_size != 0
        && payload_size <= protected_size
        && protected_size <= init_size)
}

fn has_kernel_options(path: &Path, required: &[&str], vcpus: u32) -> bool {
    let Ok(config) = fs::read_to_string(path) else {
        return false;
    };
    required
        .iter()
        .all(|required| config.lines().any(|line| line == *required))
        && (vcpus == 1
            || config
                .lines()
                .find_map(|line| line.strip_prefix("CONFIG_NR_CPUS="))
                .filter(|limit| limit.bytes().all(|byte| byte.is_ascii_digit()))
                .and_then(|limit| limit.parse::<u32>().ok())
                .is_some_and(|limit| limit >= vcpus))
}

fn configured_path(explicit: Option<&Path>, variable: &str) -> Result<Option<PathBuf>> {
    explicit
        .map(Path::to_path_buf)
        .or_else(|| env::var_os(variable).map(PathBuf::from))
        .map(|path| {
            std::path::absolute(if path.as_os_str().is_empty() {
                Path::new(".")
            } else {
                &path
            })
        })
        .transpose()
        .map_err(Into::into)
}

pub fn select_kernel(
    root: &Path,
    explicit: Option<&Path>,
    tdx: bool,
    tap: bool,
    vcpus: u32,
) -> Result<PathBuf> {
    if let Some(path) = configured_path(explicit, "SVMM_KERNEL")? {
        ensure!(
            readable_file(&path),
            "guest kernel is not readable: {}",
            path.display()
        );
        ensure!(
            !tdx || tdx_bzimage(&path).unwrap_or(false),
            "TDX guest kernel must be a complete 64-bit Linux bzImage: {}",
            path.display()
        );
        return Ok(path);
    }

    let mut required = REQUIRED_KERNEL_OPTIONS.to_vec();
    let packaged = if tdx {
        required.retain(|&entry| {
            entry != "CONFIG_X86_IOPL_IOPERM=y" && (tap || entry != "CONFIG_VIRTIO_NET=y")
        });
        required.extend([
            "CONFIG_INTEL_TDX_GUEST=y",
            "CONFIG_KVM_GUEST=y",
            "CONFIG_X86_X2APIC=y",
            "CONFIG_SWIOTLB=y",
            "CONFIG_DEVPORT=y",
        ]);
        if vcpus > 1 {
            required.push("CONFIG_SMP=y");
        }
        root.join("images/tdx-bzImage")
    } else {
        root.join("images/bzImage")
    };
    let config = packaged.with_extension("config");
    if readable_file(&packaged)
        && (!tdx
            || (tdx_bzimage(&packaged).unwrap_or(false)
                && ((!tap && vcpus == 1 && !config.exists())
                    || has_kernel_options(&config, &required, vcpus))))
    {
        return Ok(packaged);
    }

    let boot = Path::new("/boot");
    let mut newest = None;
    if let Ok(entries) = fs::read_dir(boot) {
        for entry in entries.filter_map(std::result::Result::ok) {
            let name = entry.file_name();
            let Some(version) = name.as_bytes().strip_prefix(b"vmlinuz-") else {
                continue;
            };
            let path = entry.path();
            if !readable_file(&path) || (tdx && !tdx_bzimage(&path).unwrap_or(false)) {
                continue;
            }
            let matches = |version| {
                let mut config = OsString::from("config-");
                config.push(OsStr::from_bytes(version));
                has_kernel_options(&boot.join(config), &required, vcpus)
            };
            let stripped = version
                .strip_suffix(b".bak")
                .or_else(|| version.strip_suffix(b".old"));
            if !(matches(version) || stripped.is_some_and(matches)) {
                continue;
            }
            let Ok(modified) = fs::metadata(&path).and_then(|stat| stat.modified()) else {
                continue;
            };
            if newest
                .as_ref()
                .is_none_or(|(previous, _)| modified > *previous)
            {
                newest = Some((modified, path));
            }
        }
    }
    newest
        .map(|(_, path)| path)
        .context("no compatible guest kernel found; set SVMM_KERNEL or use --kernel PATH")
}

pub fn initrd_path(root: &Path, explicit: Option<&Path>) -> Result<PathBuf> {
    let path = match configured_path(explicit, "SVMM_INITRD")? {
        Some(path) => path,
        None => {
            let path = root.join("build/workload-initramfs.cpio.gz");
            if !readable_file(&path) {
                eprintln!("svmm: preparing guest runtime");
                checked_child(
                    Command::new("make")
                        .args(["-s", "-C"])
                        .arg(root)
                        .arg("workload-initramfs"),
                    true,
                )
                .context("could not build the guest runtime; run make workload-initramfs")?;
            }
            path
        }
    };
    ensure!(
        readable_file(&path),
        "workload initramfs is not readable: {}",
        path.display()
    );
    Ok(path)
}

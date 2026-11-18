//! Static PID 1 for a Linux OCI workload guest.
mod workload_config;

use std::arch::{asm, x86_64::__cpuid};
use std::ffi::{CStr, CString};
use std::fs::{self, DirBuilder, File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt, symlink};
use std::path::Path;
use std::ptr;
use std::sync::atomic::{AtomicI32, Ordering};
use std::thread::sleep;
use std::time::Duration;

use anyhow::{Context, Result, ensure};

use workload_config::{CONFIG_HEADER_SIZE, CONFIG_MAGIC, CONFIG_MAX, ITEMS_MAX};

const EXIT_PORT: u16 = 0x604;
const EXIT_MAGIC: u32 = 0x53564d00;
const FORWARDED_SIGNALS: [libc::c_int; 3] = [libc::SIGTERM, libc::SIGINT, libc::SIGHUP];

static CHILD_PID: AtomicI32 = AtomicI32::new(0);

fn syscall(result: i32) -> io::Result<i32> {
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(result)
    }
}

fn open(path: &CStr, flags: i32) -> io::Result<OwnedFd> {
    let fd = syscall(unsafe { libc::open(path.as_ptr(), flags) })?;
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

fn mount(
    source: Option<&CStr>,
    target: &CStr,
    fs_type: Option<&CStr>,
    flags: libc::c_ulong,
    options: Option<&CStr>,
) -> Result<()> {
    let as_ptr = |value: Option<&CStr>| value.map_or(ptr::null(), CStr::as_ptr);
    syscall(unsafe {
        libc::mount(
            as_ptr(source),
            target.as_ptr(),
            as_ptr(fs_type),
            flags,
            as_ptr(options).cast(),
        )
    })
    .with_context(|| format!("mount {}", target.to_string_lossy()))?;
    Ok(())
}

fn mount_fs(kind: &CStr, target: &CStr, options: Option<&CStr>) -> Result<()> {
    mount(Some(kind), target, Some(kind), libc::MS_NOSUID, options)
}

fn make_dir(path: &str, mode: u32) -> Result<()> {
    match DirBuilder::new().mode(mode).create(path) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
        Err(error) => return Err(error).with_context(|| format!("mkdir {path}")),
    }
    ensure!(
        fs::symlink_metadata(path)?.is_dir(),
        "{path}: expected directory"
    );
    Ok(())
}

fn redirect_stdio(fd: OwnedFd) -> io::Result<()> {
    for target in 0..=2 {
        syscall(unsafe { libc::dup2(fd.as_raw_fd(), target) })?;
    }
    if fd.as_raw_fd() <= 2 {
        let _ = fd.into_raw_fd();
    }
    Ok(())
}

struct Config {
    cwd: CString,
    user: CString,
    argv: Vec<CString>,
    env: Vec<(CString, CString)>,
}

impl Config {
    fn load() -> Result<Self> {
        // Inspect the inode before opening it for I/O so a FIFO cannot block PID 1.
        let descriptor = File::from(
            open(
                c"/.svmm/workload.bin",
                libc::O_PATH | libc::O_CLOEXEC | libc::O_NOFOLLOW,
            )
            .context("open workload config")?,
        );
        let stat = descriptor.metadata().context("stat workload config")?;
        ensure!(
            stat.is_file()
                && (CONFIG_HEADER_SIZE as u64 + 2..=CONFIG_MAX as u64).contains(&stat.len()),
            "invalid workload config size"
        );
        let mut config_file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NONBLOCK)
            .open(format!("/proc/self/fd/{}", descriptor.as_raw_fd()))
            .context("read workload config")?;
        let mut data = vec![0; stat.len() as usize];
        config_file
            .read_exact(&mut data)
            .context("read workload config")?;

        ensure!(
            data.starts_with(CONFIG_MAGIC),
            "invalid workload config version"
        );
        let argc = u32::from_le_bytes(data[8..12].try_into()?) as usize;
        let envc = u32::from_le_bytes(data[12..16].try_into()?) as usize;
        ensure!(
            (1..=ITEMS_MAX).contains(&argc) && envc <= ITEMS_MAX,
            "invalid workload config counts"
        );
        let mut strings = data[CONFIG_HEADER_SIZE..].split_inclusive(|&byte| byte == 0);
        let mut next = || -> Result<CString> {
            let bytes = strings.next().context("truncated workload config")?;
            Ok(CStr::from_bytes_with_nul(bytes)
                .context("truncated workload config")?
                .to_owned())
        };
        let cwd = next()?;
        let user = next()?;
        let argv: Vec<_> = (0..argc).map(|_| next()).collect::<Result<_>>()?;
        let env = (0..envc)
            .map(|_| {
                let entry = next()?;
                let bytes = entry.to_bytes();
                let equal = bytes
                    .iter()
                    .position(|&byte| byte == b'=')
                    .filter(|&index| index > 0)
                    .context("invalid environment entry")?;
                Ok((
                    CString::new(&bytes[..equal])?,
                    CString::new(&bytes[equal + 1..])?,
                ))
            })
            .collect::<Result<_>>()?;
        ensure!(
            strings.next().is_none() && cwd.as_bytes().starts_with(b"/") && !argv[0].is_empty(),
            "invalid workload config"
        );
        Ok(Self {
            cwd,
            user,
            argv,
            env,
        })
    }
}

fn ifreq(name: &CStr) -> libc::ifreq {
    let mut request: libc::ifreq = unsafe { std::mem::zeroed() };
    for (dest, &byte) in request.ifr_name.iter_mut().zip(name.to_bytes()) {
        *dest = byte as libc::c_char;
    }
    request
}

fn network_ioctl<T>(fd: i32, request: libc::c_ulong, payload: &mut T) -> io::Result<()> {
    syscall(unsafe { libc::ioctl(fd, request as _, payload as *mut T) }).map(|_| ())
}

fn link_up(fd: i32, request: &mut libc::ifreq) -> Result<()> {
    network_ioctl(fd, libc::SIOCGIFFLAGS, request).context("read network flags")?;
    unsafe { request.ifr_ifru.ifru_flags |= libc::IFF_UP as libc::c_short };
    network_ioctl(fd, libc::SIOCSIFFLAGS, request).context("set network flags")?;
    Ok(())
}

fn ipv4(octets: [u8; 4]) -> libc::sockaddr {
    let address = libc::sockaddr_in {
        sin_family: libc::AF_INET as libc::sa_family_t,
        sin_port: 0,
        sin_addr: libc::in_addr {
            s_addr: u32::from_ne_bytes(octets),
        },
        sin_zero: [0; 8],
    };
    // Linux's interface and route ioctls store IPv4 addresses in sockaddr slots.
    unsafe { std::mem::transmute(address) }
}

fn network() -> Result<()> {
    let fd =
        syscall(unsafe { libc::socket(libc::AF_INET, libc::SOCK_DGRAM | libc::SOCK_CLOEXEC, 0) })
            .context("network socket")?;
    let socket = unsafe { OwnedFd::from_raw_fd(fd) };
    let fd = socket.as_raw_fd();
    link_up(fd, &mut ifreq(c"lo"))?;

    let mut eth = ifreq(c"eth0");
    match network_ioctl(fd, libc::SIOCGIFFLAGS, &mut eth) {
        Ok(()) => {}
        Err(error) if error.raw_os_error() == Some(libc::ENODEV) => return Ok(()),
        Err(error) => return Err(error).context("detect eth0"),
    }
    eth.ifr_ifru.ifru_addr = ipv4([192, 168, 100, 2]);
    network_ioctl(fd, libc::SIOCSIFADDR, &mut eth).context("eth0 address")?;
    eth.ifr_ifru.ifru_netmask = ipv4([255, 255, 255, 0]);
    network_ioctl(fd, libc::SIOCSIFNETMASK, &mut eth).context("eth0 netmask")?;
    link_up(fd, &mut eth)?;

    let mut route: libc::rtentry = unsafe { std::mem::zeroed() };
    route.rt_dst = ipv4([0; 4]);
    route.rt_genmask = ipv4([0; 4]);
    route.rt_gateway = ipv4([192, 168, 100, 1]);
    route.rt_flags = libc::RTF_UP | libc::RTF_GATEWAY;
    route.rt_dev = c"eth0".as_ptr().cast_mut();
    match network_ioctl(fd, libc::SIOCADDRT, &mut route) {
        Ok(()) => {}
        Err(error) if error.raw_os_error() == Some(libc::EEXIST) => {}
        Err(error) => return Err(error).context("default route"),
    }

    make_dir("/etc", 0o755)?;
    match fs::remove_file("/etc/resolv.conf") {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error).context("replace DNS config"),
    }
    fs::write("/etc/resolv.conf", b"nameserver 8.8.8.8\n").context("write DNS config")
}

fn numeric_id(bytes: &[u8]) -> Result<Option<u32>> {
    if bytes.is_empty() || !bytes.iter().all(u8::is_ascii_digit) {
        return Ok(None);
    }
    let id = std::str::from_utf8(bytes)?
        .parse()
        .context("invalid numeric user/group")?;
    ensure!(id != u32::MAX, "invalid numeric user/group");
    Ok(Some(id))
}

fn set_identity(spec: &CStr) -> Result<()> {
    let mut components = spec.to_bytes().splitn(2, |&byte| byte == b':');
    let user = components.next().unwrap_or_default();
    let (uid, user_info) = if let Some(uid) = numeric_id(user)? {
        (uid, unsafe { libc::getpwuid(uid).as_ref() })
    } else {
        let name = CString::new(user)?;
        let info =
            unsafe { libc::getpwnam(name.as_ptr()).as_ref() }.context("unknown workload user")?;
        (info.pw_uid, Some(info))
    };
    let default_gid = user_info.map_or(0, |info| info.pw_gid);
    let gid = if let Some(group) = components.next() {
        let gid = if let Some(gid) = numeric_id(group)? {
            gid
        } else {
            let name = CString::new(group)?;
            unsafe { libc::getgrnam(name.as_ptr()).as_ref() }
                .context("unknown workload group")?
                .gr_gid
        };
        syscall(unsafe { libc::setgroups(0, ptr::null()) })
            .context("clear supplementary groups")?;
        gid
    } else {
        if let Some(info) = user_info {
            let name = unsafe { CStr::from_ptr(info.pw_name) }.to_owned();
            syscall(unsafe { libc::initgroups(name.as_ptr(), default_gid) })
                .context("set supplementary groups")?;
        } else {
            syscall(unsafe { libc::setgroups(0, ptr::null()) })
                .context("clear supplementary groups")?;
        }
        default_gid
    };
    // The aliases in /dev reopen the tty through /proc/self/fd.
    syscall(unsafe { libc::fchown(libc::STDIN_FILENO, uid, gid) }).context("set terminal owner")?;
    syscall(unsafe { libc::fchmod(libc::STDIN_FILENO, 0o600) }).context("set terminal mode")?;
    syscall(unsafe { libc::setgid(gid) }).context("set workload group")?;
    syscall(unsafe { libc::setuid(uid) }).context("set workload user")?;
    Ok(())
}

extern "C" fn forward_signal(signum: i32) {
    let pid = CHILD_PID.load(Ordering::Relaxed);
    if pid > 0 {
        unsafe { libc::kill(-pid, signum) };
    }
}

fn enable_signal_forwarding() -> Result<()> {
    let mut action: libc::sigaction = unsafe { std::mem::zeroed() };
    action.sa_sigaction = forward_signal as *const () as libc::sighandler_t;
    unsafe { libc::sigemptyset(&mut action.sa_mask) };
    for signal in FORWARDED_SIGNALS {
        syscall(unsafe { libc::sigaction(signal, &action, ptr::null_mut()) })
            .context("install workload signal handler")?;
    }
    Ok(())
}

enum Report {
    Unavailable,
    Port,
    Tdx(File),
}

impl Report {
    #[allow(unused_unsafe)] // CPUID was unsafe on earlier supported Rust versions.
    fn setup() -> Result<Self> {
        let max = unsafe { __cpuid(0) }.eax;
        if max >= 0x21 {
            let vendor = unsafe { __cpuid(0x21) };
            if (vendor.ebx, vendor.edx, vendor.ecx) == (0x65746e49, 0x5844546c, 0x20202020) {
                // A kernel outb through /dev/port is allowed in TDX guests.
                let fd = open(c"/dev/port", libc::O_WRONLY | libc::O_CLOEXEC)
                    .context("open TD workload completion port")?;
                return Ok(Self::Tdx(File::from(fd)));
            }
        }
        syscall(unsafe { libc::ioperm(EXIT_PORT.into(), 4, 1) })
            .context("enable workload report")?;
        Ok(Self::Port)
    }

    fn close_for_child(self) -> Result<()> {
        // Dropping the TDX File closes the child's copy of /dev/port.
        if matches!(self, Self::Port) {
            syscall(unsafe { libc::ioperm(EXIT_PORT.into(), 4, 0) })
                .context("revoke workload report")?;
        }
        Ok(())
    }

    fn send(&mut self, status: u8) -> io::Result<()> {
        match self {
            Self::Port => unsafe {
                asm!("out dx, eax", in("dx") EXIT_PORT, in("eax") EXIT_MAGIC | u32::from(status), options(nomem, nostack, preserves_flags));
            },
            Self::Tdx(file) => {
                file.seek(SeekFrom::Start(EXIT_PORT.into()))?;
                file.write_all(&[b'S', b'V', b'M', status])?;
            }
            Self::Unavailable => {}
        }
        Ok(())
    }
}

struct Guest {
    report: Report,
    root_mounted: bool,
}

impl Guest {
    fn run(&mut self) -> Result<i32> {
        mount_fs(c"devtmpfs", c"/dev", None)?;
        if let Ok(console) = open(c"/dev/console", libc::O_RDWR | libc::O_NOCTTY) {
            redirect_stdio(console).context("redirect guest console")?;
        }
        self.report = Report::setup()?;
        mount_fs(c"proc", c"/proc", None)?;
        mount_fs(c"sysfs", c"/sys", None)?;
        for _ in 0..500 {
            if Path::new("/dev/vda").exists() {
                break;
            }
            sleep(Duration::from_millis(10));
        }

        mount(Some(c"/dev/vda"), c"/rootfs", Some(c"ext4"), 0, None)?;
        for (source, target) in [
            (c"/dev", c"/rootfs/dev"),
            (c"/proc", c"/rootfs/proc"),
            (c"/sys", c"/rootfs/sys"),
        ] {
            make_dir(target.to_str()?, 0o755)?;
            mount(Some(source), target, None, libc::MS_MOVE, None)?;
        }
        syscall(unsafe { libc::chdir(c"/rootfs".as_ptr()) }).context("enter rootfs directory")?;
        syscall(unsafe { libc::chroot(c".".as_ptr()) }).context("enter rootfs")?;
        syscall(unsafe { libc::chdir(c"/".as_ptr()) }).context("enter root directory")?;
        self.root_mounted = true;

        make_dir("/dev/pts", 0o755)?;
        mount_fs(c"devpts", c"/dev/pts", Some(c"mode=0620,ptmxmode=0666"))?;
        for (alias, target) in [
            ("/dev/fd", "/proc/self/fd"),
            ("/dev/stdin", "/proc/self/fd/0"),
            ("/dev/stdout", "/proc/self/fd/1"),
            ("/dev/stderr", "/proc/self/fd/2"),
        ] {
            match symlink(target, alias) {
                Ok(()) => {}
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
                Err(error) => return Err(error).context("create standard device alias"),
            }
        }
        make_dir("/dev/shm", 0o1777)?;
        mount_fs(c"tmpfs", c"/dev/shm", Some(c"mode=1777"))?;
        make_dir("/run", 0o755)?;
        make_dir("/tmp", 0o1777)?;
        fs::set_permissions("/tmp", fs::Permissions::from_mode(0o1777)).context("set /tmp mode")?;

        let config = Config::load()?;
        DirBuilder::new()
            .recursive(true)
            .mode(0o755)
            .create(Path::new(std::ffi::OsStr::from_bytes(
                config.cwd.to_bytes(),
            )))
            .context("create working directory")?;
        network()?;
        enable_signal_forwarding()?;

        let pid = syscall(unsafe { libc::fork() }).context("fork workload")?;
        if pid == 0 {
            let report = std::mem::replace(&mut self.report, Report::Unavailable);
            let status = match Self::execute(config, report) {
                Ok(status) => status,
                Err(error) => {
                    eprintln!("svmm-init: {error:#}");
                    125
                }
            };
            unsafe { libc::_exit(status) };
        }
        CHILD_PID.store(pid, Ordering::Relaxed);
        let status = loop {
            let mut status = 0;
            match syscall(unsafe { libc::waitpid(-1, &mut status, 0) }) {
                Ok(reaped) if reaped == pid => break status,
                Ok(_) => {}
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(error) => return Err(error).context("wait workload"),
            }
        };
        CHILD_PID.store(0, Ordering::Relaxed);
        Ok(if libc::WIFEXITED(status) {
            libc::WEXITSTATUS(status)
        } else {
            128 + libc::WTERMSIG(status)
        })
    }

    fn execute(config: Config, report: Report) -> Result<i32> {
        syscall(unsafe { libc::setsid() }).context("setsid")?;
        let tty = open(c"/dev/ttyS0", libc::O_RDWR).context("open guest tty")?;
        syscall(unsafe { libc::ioctl(tty.as_raw_fd(), libc::TIOCSCTTY, 0) })
            .context("set controlling tty")?;
        redirect_stdio(tty).context("redirect workload stdio")?;

        for signal in FORWARDED_SIGNALS.into_iter().chain([libc::SIGPIPE]) {
            unsafe { libc::signal(signal, libc::SIG_DFL) };
        }
        report.close_for_child()?;
        set_identity(&config.user)?;
        syscall(unsafe { libc::chdir(config.cwd.as_ptr()) }).context("workload cwd")?;
        syscall(unsafe { libc::clearenv() }).context("clear workload environment")?;
        for (key, value) in &config.env {
            syscall(unsafe { libc::setenv(key.as_ptr(), value.as_ptr(), 1) })
                .context("set workload environment")?;
        }
        let argv: Vec<_> = config
            .argv
            .iter()
            .map(|arg| arg.as_ptr())
            .chain([ptr::null()])
            .collect();
        unsafe { libc::execvp(argv[0], argv.as_ptr()) };
        let error = io::Error::last_os_error();
        eprintln!("svmm-init: exec workload: {error}");
        Ok(if error.raw_os_error() == Some(libc::ENOENT) {
            127
        } else {
            126
        })
    }

    fn finish(&mut self, status: i32) -> ! {
        // PID 1 is excluded by kill(-1). Reap remaining application descendants.
        unsafe { libc::kill(-1, libc::SIGTERM) };
        'grace: for _ in 0..20 {
            loop {
                match syscall(unsafe { libc::waitpid(-1, ptr::null_mut(), libc::WNOHANG) }) {
                    Ok(0) => break,
                    Ok(_) => {}
                    Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                    Err(_) => break 'grace,
                }
            }
            sleep(Duration::from_millis(10));
        }
        unsafe { libc::kill(-1, libc::SIGKILL) };
        loop {
            match syscall(unsafe { libc::waitpid(-1, ptr::null_mut(), 0) }) {
                Ok(_) => {}
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(_) => break,
            }
        }
        unsafe { libc::sync() };
        if self.root_mounted {
            if let Err(error) = mount(None, c"/", None, libc::MS_REMOUNT | libc::MS_RDONLY, None) {
                eprintln!("svmm-init: remount root read-only: {error:#}");
            }
        }
        eprintln!("svmm-init: workload exited status={status}");
        if let Err(error) = self.report.send(status as u8) {
            eprintln!("svmm-init: send workload completion: {error}");
        }
        unsafe { libc::reboot(libc::RB_POWER_OFF) };
        loop {
            unsafe { libc::pause() };
        }
    }
}

fn main() -> Result<()> {
    ensure!(
        unsafe { libc::getpid() } == 1,
        "svmm-init must run as PID 1"
    );
    let mut guest = Guest {
        report: Report::Unavailable,
        root_mounted: false,
    };
    let status = match guest.run() {
        Ok(status) => status,
        Err(error) => {
            eprintln!("svmm-init: {error:#}");
            125
        }
    };
    guest.finish(status);
}

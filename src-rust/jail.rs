use anyhow::{Context, Result, bail};
use std::{
    fs, io,
    path::{Path, PathBuf},
};

#[derive(Default)]
pub struct Profile {
    pub cpu_max: u64,
    pub memory: u64,
    pub nofile: u64,
    pub nproc: u64,
}

impl Profile {
    pub fn finish(&mut self, guest_mib: usize) {
        if self.cpu_max == 0 {
            self.cpu_max = 100_000;
        }
        if self.memory == 0 {
            self.memory = guest_mib as u64 + 128;
        }
        if self.nofile == 0 {
            self.nofile = 64;
        }
        if self.nproc == 0 {
            self.nproc = 32;
        }
    }

    pub fn limit(&self) -> Result<()> {
        match self.cgroup() {
            Ok(path) => eprintln!(
                "INFO: jail: cgroup applied ({} cpu.max={}/100000 memory.max={}M pids.max={})",
                path.display(),
                self.cpu_max,
                self.memory,
                self.nproc
            ),
            Err(error) => {
                eprintln!("WARN: jail: cgroup unavailable ({error:#}), continuing without it")
            }
        }
        for (kind, value, name) in [
            (libc::RLIMIT_NOFILE, self.nofile, "RLIMIT_NOFILE"),
            (libc::RLIMIT_NPROC, self.nproc, "RLIMIT_NPROC"),
        ] {
            let limit = libc::rlimit {
                rlim_cur: value,
                rlim_max: value,
            };
            cvt(unsafe { libc::setrlimit(kind, &limit) })
                .with_context(|| format!("setrlimit({name})"))?;
        }
        eprintln!(
            "INFO: jail: rlimit applied (nofile={} nproc={})",
            self.nofile, self.nproc
        );
        Ok(())
    }

    fn cgroup(&self) -> Result<PathBuf> {
        let root = Path::new("/sys/fs/cgroup");
        let mut stat = std::mem::MaybeUninit::<libc::statfs>::uninit();
        cvt(unsafe { libc::statfs(c"/sys/fs/cgroup".as_ptr(), stat.as_mut_ptr()) })?;
        if unsafe { stat.assume_init() }.f_type != 0x6367_7270 {
            bail!("{} is not a cgroup v2 mount", root.display());
        }
        let parent = root.join("svmm");
        fs::create_dir_all(&parent).context("mkdir svmm")?;
        for path in [root, parent.as_path()] {
            fs::write(path.join("cgroup.subtree_control"), "+cpu +memory +pids")
                .context("enable controllers")?;
        }
        for entry in fs::read_dir(&parent)?.flatten() {
            let name = entry.file_name();
            let Some(pid) = name
                .to_str()
                .and_then(|s| s.strip_prefix("vm-"))
                .and_then(|s| s.parse::<i32>().ok())
            else {
                continue;
            };
            if pid > 0
                && unsafe { libc::kill(pid, 0) } < 0
                && io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH)
            {
                let _ = fs::remove_dir(entry.path());
            }
        }
        let pid = std::process::id();
        let path = parent.join(format!("vm-{pid}"));
        fs::create_dir_all(&path)?;
        let apply = || -> Result<()> {
            fs::write(path.join("cpu.max"), format!("{} 100000", self.cpu_max))
                .context("cpu.max")?;
            fs::write(path.join("memory.max"), (self.memory << 20).to_string())
                .context("memory.max")?;
            if let Err(error) = fs::write(path.join("memory.swap.max"), "0") {
                if error.kind() != io::ErrorKind::NotFound {
                    return Err(error).context("memory.swap.max");
                }
            }
            fs::write(path.join("pids.max"), self.nproc.to_string()).context("pids.max")?;
            fs::write(path.join("cgroup.procs"), pid.to_string()).context("cgroup.procs")?;
            Ok(())
        };
        if let Err(error) = apply() {
            let _ = fs::remove_dir(&path);
            return Err(error);
        }
        Ok(path)
    }
}

fn cvt(result: libc::c_int) -> io::Result<()> {
    if result == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

pub fn drop_caps() -> Result<()> {
    cvt(unsafe {
        libc::prctl(
            libc::PR_CAP_AMBIENT,
            libc::PR_CAP_AMBIENT_CLEAR_ALL,
            0,
            0,
            0,
        )
    })
    .context("clear ambient capabilities")?;
    for cap in 0..64 {
        let result = unsafe { libc::prctl(libc::PR_CAPBSET_DROP, cap, 0, 0, 0) };
        if let Err(error) = cvt(result) {
            if error.raw_os_error() == Some(libc::EINVAL) {
                break;
            }
            return Err(error).context("drop bounding capability");
        }
    }
    // Linux capability API version 3, two 32-bit words per capability set.
    let header = [0x2008_0522u32, 0];
    let data = [0u32; 6];
    let result = unsafe { libc::syscall(libc::SYS_capset, header.as_ptr(), data.as_ptr()) };
    cvt(result as i32).context("capset")?;
    eprintln!(
        "INFO: jail: capabilities dropped (effective, permitted, inheritable, bounding, ambient)"
    );
    Ok(())
}

pub fn install_seccomp() -> Result<()> {
    const LOAD: u16 = 0x20;
    const EQUAL: u16 = 0x15;
    const BIT_SET: u16 = 0x45;
    const RETURN: u16 = 0x06;
    const KILL: u32 = 0x8000_0000;
    const ALLOW: u32 = 0x7fff_0000;
    const ARCH_X86_64: u32 = 0xc000_003e;
    let instruction = |code, jt, jf, k| libc::sock_filter { code, jt, jf, k };
    let allowed = [
        libc::SYS_read,
        libc::SYS_write,
        libc::SYS_readv,
        libc::SYS_writev,
        libc::SYS_pread64,
        libc::SYS_pwrite64,
        libc::SYS_fdatasync,
        libc::SYS_fsync,
        libc::SYS_close,
        libc::SYS_fstat,
        libc::SYS_ioctl,
        libc::SYS_poll,
        libc::SYS_ppoll,
        libc::SYS_futex,
        libc::SYS_sched_yield,
        libc::SYS_munmap,
        libc::SYS_mremap,
        libc::SYS_madvise,
        libc::SYS_brk,
        libc::SYS_rt_sigprocmask,
        libc::SYS_rt_sigreturn,
        libc::SYS_tgkill,
        libc::SYS_getpid,
        libc::SYS_gettid,
        libc::SYS_clock_gettime,
        libc::SYS_getrandom,
        libc::SYS_set_robust_list,
        libc::SYS_rseq,
        // The Rust runtime installs and releases a per-thread signal stack.
        libc::SYS_sigaltstack,
        libc::SYS_sched_getaffinity,
        libc::SYS_restart_syscall,
        libc::SYS_exit,
        libc::SYS_exit_group,
    ];
    let mut filter = vec![
        instruction(LOAD, 0, 0, 4),
        instruction(EQUAL, 1, 0, ARCH_X86_64),
        instruction(RETURN, 0, 0, KILL),
        instruction(LOAD, 0, 0, 0),
        instruction(BIT_SET, 0, 1, 0x4000_0000),
        instruction(RETURN, 0, 0, KILL),
    ];
    for call in [libc::SYS_mmap, libc::SYS_mprotect] {
        filter.extend([
            instruction(EQUAL, 0, 4, call as u32),
            instruction(LOAD, 0, 0, 16 + 2 * 8),
            instruction(BIT_SET, 0, 1, libc::PROT_EXEC as u32),
            instruction(RETURN, 0, 0, KILL),
            instruction(RETURN, 0, 0, ALLOW),
        ]);
    }
    // Thread initialization can reach pthread_setname_np after TSYNC.
    // Permit naming the calling thread without permitting other prctl options.
    filter.extend([
        instruction(EQUAL, 0, 4, libc::SYS_prctl as u32),
        instruction(LOAD, 0, 0, 16),
        instruction(EQUAL, 0, 1, libc::PR_SET_NAME as u32),
        instruction(RETURN, 0, 0, ALLOW),
        instruction(RETURN, 0, 0, KILL),
    ]);
    for call in allowed {
        filter.push(instruction(EQUAL, 0, 1, call as u32));
        filter.push(instruction(RETURN, 0, 0, ALLOW));
    }
    filter.push(instruction(RETURN, 0, 0, KILL));
    let program = libc::sock_fprog {
        len: filter.len() as u16,
        filter: filter.as_mut_ptr(),
    };
    cvt(unsafe { libc::prctl(libc::PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) })
        .context("PR_SET_NO_NEW_PRIVS")?;
    let result = unsafe { libc::syscall(libc::SYS_seccomp, 1u32, 1u32, &program) };
    if result > 0 {
        bail!("seccomp TSYNC failed on thread {result}");
    }
    cvt(result as i32).context("seccomp TSYNC")?;
    eprintln!(
        "INFO: jail: seccomp installed ({} allowed syscalls, {} instructions, TSYNC)",
        allowed.len(),
        filter.len()
    );
    Ok(())
}

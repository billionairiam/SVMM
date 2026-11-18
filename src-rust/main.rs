mod cli;
mod devices;
mod jail;
mod machine;
mod memory;
mod metrics;
mod oci;
mod runtime;
mod snapshot;
mod vmstate;
mod workload_config;

use anyhow::{Context, Result, ensure};
use cli::{Action, Run, Snapshot};
use std::{
    env,
    os::unix::process::CommandExt,
    path::{Path, PathBuf},
    process::{Command, ExitCode},
    sync::atomic::{AtomicBool, AtomicI32, Ordering},
    time::Duration,
};

static STOP_SIGNAL: AtomicI32 = AtomicI32::new(0);
static SAVE_SIGNAL: AtomicBool = AtomicBool::new(false);

pub fn cancelled() -> Option<i32> {
    let signal = STOP_SIGNAL.load(Ordering::Acquire);
    (signal != 0).then_some(signal)
}

fn request_stop(signal: i32) {
    STOP_SIGNAL.store(signal, Ordering::Release);
}

extern "C" fn signal_handler(signal: libc::c_int) {
    match signal {
        libc::SIGUSR1 => {}
        libc::SIGUSR2 => SAVE_SIGNAL.store(true, Ordering::Release),
        _ => request_stop(signal),
    }
}

fn signals(warm_save: bool) -> Result<()> {
    let mut action = unsafe { std::mem::zeroed::<libc::sigaction>() };
    action.sa_sigaction = signal_handler as *const () as usize;
    unsafe {
        libc::sigemptyset(&mut action.sa_mask);
    }
    for signal in [libc::SIGUSR1, libc::SIGINT, libc::SIGTERM, libc::SIGHUP] {
        ensure!(
            unsafe { libc::sigaction(signal, &action, std::ptr::null_mut()) } == 0,
            "install signal handler: {}",
            std::io::Error::last_os_error()
        );
    }
    if warm_save {
        ensure!(
            unsafe { libc::sigaction(libc::SIGUSR2, &action, std::ptr::null_mut()) } == 0,
            "install snapshot handler: {}",
            std::io::Error::last_os_error()
        );
    }
    let mut wake = unsafe { std::mem::zeroed::<libc::sigset_t>() };
    unsafe {
        libc::sigemptyset(&mut wake);
        libc::sigaddset(&mut wake, libc::SIGUSR1);
        if warm_save {
            libc::sigaddset(&mut wake, libc::SIGUSR2);
        }
    }
    let error = unsafe { libc::pthread_sigmask(libc::SIG_UNBLOCK, &wake, std::ptr::null_mut()) };
    if error != 0 {
        return Err(std::io::Error::from_raw_os_error(error)).context("unblock VMM signals");
    }
    Ok(())
}

fn resources() -> Result<PathBuf> {
    let binary = env::current_exe()?;
    let dir = binary
        .parent()
        .context("locate svmm executable directory")?;
    // Installed bin/svmm ships next to the images and rootfs directories.
    if dir.file_name().is_some_and(|name| name == "bin") {
        return Ok(dir.parent().unwrap_or(dir).to_owned());
    }
    dir.ancestors()
        .find(|ancestor| ancestor.join("Cargo.toml").is_file() && ancestor.join("rootfs").is_dir())
        .map(Path::to_owned)
        .context("locate svmm resources")
}

fn configure_firmware(config: &mut Run) -> Result<()> {
    if config.tdx && config.firmware.is_none() {
        config.firmware = Some(
            match env::var_os("SVMM_TD_SHIM").filter(|value| !value.is_empty()) {
                Some(path) => PathBuf::from(path),
                None => resources()?.join("images/td-shim.bin"),
            },
        );
    }
    Ok(())
}

fn drive_image(config: &Run) -> Result<i32> {
    ensure!(
        unsafe { libc::geteuid() } == 0,
        "run --image as root to preserve image file ownership"
    );
    if let Some(firmware) = &config.firmware {
        machine::tdx_preflight(firmware, config.vcpus)?;
    }
    let root = resources()?;
    let kernel = oci::select_kernel(
        &root,
        config.kernel.as_deref(),
        config.tdx,
        config.tap.is_some(),
        config.vcpus,
    )?;
    let initrd = oci::initrd_path(&root, config.initrd.as_deref())?;
    let options = config.image.as_ref().unwrap();
    eprintln!(
        "svmm: image {}; guest kernel {}",
        options.image,
        kernel.display()
    );
    let prepared = oci::prepare(options, &config.command)?;
    if let Some(signal) = cancelled() {
        return Ok(128 + signal);
    }

    // The image coordinator must keep filesystem access for cleanup while a
    // jailed child drops capabilities and installs its syscall filter.
    let mut command = Command::new(env::current_exe()?);
    command
        .arg("run")
        .arg("--kernel")
        .arg(kernel)
        .arg("--initrd")
        .arg(initrd)
        .arg("--disk")
        .arg(&prepared.disk)
        .arg("--workload")
        .arg("--mem")
        .arg(config.mem.to_string())
        .arg("--cmdline")
        .arg(&config.cmdline);
    if let Some(tap) = &config.tap {
        command.arg("--tap").arg(tap);
    }
    if let Some(netns) = &config.netns {
        command.arg("--netns").arg(netns);
    }
    if let Some(name) = &config.save {
        command.arg("--warm-save").arg(name);
        // Keep a forwarded save pending across exec until the child can
        // handle it. Preparation-stage requests are discarded before spawn.
        unsafe {
            command.pre_exec(|| {
                let mut set = std::mem::zeroed::<libc::sigset_t>();
                libc::sigemptyset(&mut set);
                libc::sigaddset(&mut set, libc::SIGUSR2);
                if libc::sigprocmask(libc::SIG_BLOCK, &set, std::ptr::null_mut()) != 0 {
                    return Err(std::io::Error::last_os_error());
                }
                Ok(())
            });
        }
    }
    if let Some(firmware) = &config.firmware {
        command
            .arg("--tdx")
            .arg("--td-shim")
            .arg(firmware)
            .arg("--vcpus")
            .arg(config.vcpus.to_string());
    }
    if let Some(profile) = &config.jail {
        command
            .arg("--jail")
            .arg("--cpu-max")
            .arg(profile.cpu_max.to_string())
            .arg("--memory-max")
            .arg(profile.memory.to_string())
            .arg("--nofile")
            .arg(profile.nofile.to_string())
            .arg("--nproc")
            .arg(profile.nproc.to_string());
    }
    SAVE_SIGNAL.store(false, Ordering::Release);
    let mut child = command.spawn().context("start workload VM")?;
    let mut forwarded = None;
    loop {
        if let Some(status) = child.try_wait()? {
            use std::os::unix::process::ExitStatusExt;
            return Ok(status
                .code()
                .or_else(|| status.signal().map(|signal| 128 + signal))
                .unwrap_or(125));
        }
        if let Some(signal) = cancelled() {
            if forwarded != Some(signal) {
                unsafe {
                    libc::kill(child.id() as i32, signal);
                }
                forwarded = Some(signal);
            }
        } else if SAVE_SIGNAL.swap(false, Ordering::AcqRel) {
            unsafe {
                libc::kill(child.id() as i32, libc::SIGUSR2);
            }
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn run(mut config: Box<Run>) -> i32 {
    let image = config.image.is_some();
    let workload = config.workload || image;
    let result = (|| {
        signals(config.save.is_some())?;
        configure_firmware(&mut config)?;
        if image {
            drive_image(&config)
        } else {
            runtime::launch(&config)
        }
    })();
    match result {
        Ok(code) => code,
        Err(error) => {
            eprintln!("ERROR: {error:#}");
            if image {
                if let Some(signal) = cancelled() {
                    return 128 + signal;
                }
            }
            if workload { 125 } else { 1 }
        }
    }
}

fn main() -> ExitCode {
    let args: Option<Vec<_>> = env::args_os()
        .skip(1)
        .map(|arg| arg.into_string().ok())
        .collect();
    let action = args
        .as_deref()
        .context("arguments must be valid UTF-8")
        .and_then(cli::parse);
    let code = match action {
        Err(error) => {
            eprintln!("ERROR: {error:#}\nTry 'svmm run --help' for more information.");
            2
        }
        Ok(Action::Help(help)) => {
            print!("{help}");
            0
        }
        Ok(Action::Run(config)) => run(config),
        Ok(Action::Build(options, command, output)) => {
            let result = signals(false).and_then(|_| oci::build(&options, &command, &output));
            match (result, cancelled()) {
                (Err(_), Some(signal)) => 128 + signal,
                (result, _) => report(result),
            }
        }
        Ok(Action::Snapshot(operation)) => report(
            match operation {
                Snapshot::List => snapshot::list(),
                Snapshot::Save(name, disk, info) => snapshot::save(&name, &disk, info),
                Snapshot::Validate(name) => snapshot::validate(&name),
                Snapshot::Delete(name) => snapshot::delete(&name),
            }
            .context("snapshot"),
        ),
    };
    ExitCode::from(code as u8)
}

fn report(result: Result<()>) -> i32 {
    match result {
        Ok(()) => 0,
        Err(error) => {
            eprintln!("ERROR: {error:#}");
            1
        }
    }
}

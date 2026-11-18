use crate::{
    SAVE_SIGNAL, cancelled,
    cli::Run,
    devices::Devices,
    jail,
    machine::{BootConfig, Machine},
    memory::{InvalidMapGpa, Memory},
    metrics::Stats,
    snapshot, vmstate,
};
use anyhow::{Context, Result, bail, ensure};
use kvm_ioctls::{VcpuExit, VcpuFd, VmFd};
use std::{
    fs::File,
    io::{self, Write},
    os::{
        fd::{AsRawFd, FromRawFd},
        unix::thread::JoinHandleExt,
    },
    path::Path,
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread::{self, JoinHandle},
    time::Instant,
};

const MIB: usize = 1024 * 1024;

pub fn stage(name: &str, bytes: usize) {
    eprintln!("SVMM_METRIC variant=baseline stage={name} guest_memory_bytes={bytes}");
}

fn enter_namespace(name: &str) -> Result<()> {
    ensure!(
        crate::cli::identifier(name, 64, true),
        "invalid netns name '{name}'"
    );
    let path = Path::new("/var/run/netns").join(name);
    let file = File::open(&path)
        .with_context(|| format!("cannot open netns {}", path.display()))
        .map_err(|error| {
            match error
                .downcast_ref::<io::Error>()
                .and_then(io::Error::raw_os_error)
            {
                Some(libc::EACCES | libc::EPERM) => {
                    error.context("entering a network namespace requires CAP_SYS_ADMIN")
                }
                Some(libc::ENOENT) => {
                    error.context(format!("create the namespace with 'ip netns add {name}'"))
                }
                _ => error,
            }
        })?;
    if unsafe { libc::setns(file.as_raw_fd(), libc::CLONE_NEWNET) } != 0 {
        let error = io::Error::last_os_error();
        return match error.raw_os_error() {
            Some(libc::EINVAL) => {
                Err(error).context(format!("{} is not a network namespace", path.display()))
            }
            Some(libc::EPERM | libc::EACCES) => {
                Err(error).context("entering a network namespace requires CAP_SYS_ADMIN")
            }
            _ => Err(error).context(format!("setns {}", path.display())),
        };
    }
    eprintln!("INFO: entered network namespace {name}");
    Ok(())
}

pub fn launch(config: &Run) -> Result<i32> {
    let start = Instant::now();
    if let Some(name) = &config.netns {
        enter_namespace(name).inspect_err(|_| stage("failed_netns", 0))?;
    }
    if let Some(profile) = &config.jail {
        profile.limit()?;
    }
    if let Some(name) = &config.save {
        snapshot::check_new(name)?;
    }
    if let (Some(name), Some(disk)) = (&config.cold, &config.disk) {
        snapshot::restore_cold(name, disk).inspect_err(|_| stage("failed_snapshot", 0))?;
    }

    let (mut machine, devices, mem) = if let Some(name) = &config.restore {
        let restored = (|| -> Result<_> {
            let warm = snapshot::Warm::open(name, config.disk.is_some(), config.tap.is_some())?;
            let state =
                vmstate::State::decode(&warm.state, config.disk.is_some(), config.tap.is_some())?;
            let machine = Machine::restore(warm.mem_mib, &warm.memory)?;
            warm.verify_mapping(&machine.memory)?;
            state.apply(&machine.vm, &machine.vcpus[0])?;
            let pending = config
                .disk
                .as_deref()
                .map(|disk| warm.prepare_disk(disk))
                .transpose()?;
            let devices = Devices::new(
                machine.memory.clone(),
                machine.vm.clone(),
                pending.as_ref().map(snapshot::PendingDisk::path),
                config.tap.as_deref(),
                false,
            )?;
            devices.restore_sections(&state.devices())?;
            if let Some(pending) = pending {
                pending.publish()?;
            }
            Ok((machine, devices, warm.mem_mib))
        })()
        .inspect_err(|_| stage("failed_restore", 0))?;
        eprintln!(
            "INFO: warm snapshot '{name}' restored in {:.1} ms",
            start.elapsed().as_secs_f64() * 1000.0
        );
        stage("restored", restored.2 * MIB);
        restored
    } else {
        let boot = BootConfig {
            kernel: config.kernel.as_deref().context("missing guest kernel")?,
            initrd: config.initrd.as_deref(),
            cmdline: &config.cmdline,
            mem_mib: config.mem,
            vcpus: config.vcpus,
            td_shim: config.firmware.as_deref(),
            disk: config.disk.is_some(),
            net: config.tap.is_some(),
        };
        let machine = Machine::boot(&boot)?;
        let devices = Devices::new(
            machine.memory.clone(),
            machine.vm.clone(),
            config.disk.as_deref(),
            config.tap.as_deref(),
            config.tdx,
        )
        .inspect_err(|error| {
            if let Some(open) = error.downcast_ref::<crate::devices::OpenError>() {
                stage(open.stage, config.mem * MIB);
            }
        })?;
        (machine, devices, config.mem)
    };

    if config.jail.is_some() {
        jail::drop_caps()?;
    }
    let devices = Arc::new(devices);
    let console = Console::start(devices.clone(), config.save.is_some())?;
    let mut rx = devices.start_rx()?;
    let control = Arc::new(Control {
        stopped: AtomicBool::new(false),
        frame: Mutex::new(0),
        workload: config.workload,
        tdx: config.tdx,
    });

    let result = loop {
        let (outcome, vcpus, stats) = match run_vcpus(
            &mut machine,
            &devices,
            &control,
            config.jail.is_some(),
            mem * MIB,
        ) {
            Ok(value) => value,
            Err(error) => break Err(error),
        };
        machine.vcpus = vcpus;

        if matches!(outcome, Outcome::Save) {
            stop_rx(&devices, rx.take());
            let name = config
                .save
                .as_deref()
                .expect("save mode requested with a configured snapshot");
            let saved = save_vm(config, name, &mut machine, &devices, mem);
            if let Err(error) = saved {
                eprintln!("ERROR: warm snapshot '{name}' failed; the VM keeps running: {error:#}");
                control.stopped.store(false, Ordering::Release);
                match devices.start_rx() {
                    Ok(handle) => rx = handle,
                    Err(error) => break Err(error),
                }
                continue;
            }
        }

        eprintln!(
            "SVMM_METRIC variant=baseline stage=vcpu_exit reason={} entered={} exits={} serial_exits={}",
            outcome.reason(),
            u8::from(stats.exits != 0),
            stats.exits,
            stats.serial
        );
        stats.print();
        break match outcome {
            Outcome::Workload(code) => Ok(code),
            Outcome::Save | Outcome::HostStop => Ok(0),
            Outcome::Guest(_) if config.workload => bail_missing_completion(),
            Outcome::Guest(_) => Ok(0),
            Outcome::Failed(_, error) => Err(error),
        };
    };
    drop(console);
    stop_rx(&devices, rx);
    devices.print_stats();
    if matches!(result, Ok(0)) {
        eprintln!("INFO: sandbox completed");
    }
    result
}

fn bail_missing_completion() -> Result<i32> {
    bail!("guest stopped without a workload exit report")
}

fn stop_rx(devices: &Devices, worker: Option<JoinHandle<Result<()>>>) {
    if let Some(worker) = worker {
        if let Err(error) = devices.stop_rx() {
            eprintln!("ERROR: stop virtio RX: {error:#}");
        }
        match worker.join() {
            Ok(Err(error)) => eprintln!("ERROR: virtio RX: {error:#}"),
            Err(_) => eprintln!("ERROR: virtio RX worker panicked"),
            _ => {}
        }
    }
}

fn save_vm(
    config: &Run,
    name: &str,
    machine: &mut Machine,
    devices: &Devices,
    mem: usize,
) -> Result<()> {
    let vcpu = &mut machine.vcpus[0];
    vcpu.set_kvm_immediate_exit(1);
    let quiesced = vcpu
        .run()
        .err()
        .is_some_and(|error| error.errno() == libc::EINTR);
    vcpu.set_kvm_immediate_exit(0);
    ensure!(quiesced, "could not quiesce vCPU");
    let state = devices.snapshot_with(|sections| vmstate::capture(&machine.vm, vcpu, sections))?;
    let disk = devices.disk_file()?;
    let info = snapshot::Info {
        kernel: config
            .kernel
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned()),
        initrd: config
            .initrd
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned()),
        cmdline: Some(config.cmdline.clone()),
        mem: Some(mem),
    };
    snapshot::save_warm(
        name,
        info,
        &machine.memory,
        config.tap.is_some(),
        &state,
        disk.as_ref().zip(config.disk.as_deref()),
    )
}

struct Control {
    stopped: AtomicBool,
    frame: Mutex<usize>,
    workload: bool,
    tdx: bool,
}

enum Outcome {
    Workload(i32),
    Guest(&'static str),
    HostStop,
    Save,
    Failed(&'static str, anyhow::Error),
}

impl Outcome {
    fn reason(&self) -> &'static str {
        match self {
            Self::Workload(_) => "workload_exit",
            Self::Guest(label) => label,
            Self::HostStop => "host_stop",
            Self::Save => "warm_save",
            Self::Failed(label, _) => label,
        }
    }
}

fn run_vcpus(
    machine: &mut Machine,
    devices: &Arc<Devices>,
    control: &Arc<Control>,
    jailed: bool,
    memory_bytes: usize,
) -> Result<(Outcome, Vec<VcpuFd>, Stats)> {
    let gate = Arc::new((Mutex::new(false), Condvar::new()));
    let (sender, receiver) = mpsc::channel();
    let count = machine.vcpus.len();
    let mut workers = Vec::with_capacity(count);
    let mut failure = None;
    for (index, mut vcpu) in std::mem::take(&mut machine.vcpus).into_iter().enumerate() {
        let (gate, vm, memory, devices, control, sender) = (
            gate.clone(),
            machine.vm.clone(),
            machine.memory.clone(),
            devices.clone(),
            control.clone(),
            sender.clone(),
        );
        match thread::Builder::new()
            .name(format!("svmm-vcpu-{index}"))
            .spawn(move || {
                let (ready, wake) = &*gate;
                drop(
                    wake.wait_while(ready.lock().unwrap(), |ready| !*ready)
                        .unwrap(),
                );
                let mut stats = Stats::default();
                let result = run_cpu(&mut vcpu, &vm, &memory, &devices, &control, &mut stats);
                match result {
                    Ok(Some(outcome)) => {
                        let _ = sender.send(outcome);
                    }
                    Ok(None) => {}
                    Err(error) => {
                        let _ = sender.send(Outcome::Failed("kvm_error", error));
                    }
                }
                (vcpu, stats)
            }) {
            Ok(worker) => workers.push(worker),
            Err(error) => {
                failure = Some(anyhow::Error::new(error).context("start vCPU thread"));
                break;
            }
        }
    }
    drop(sender);

    if failure.is_none() && jailed {
        failure = jail::install_seccomp().err();
    }
    if failure.is_some() {
        control.stopped.store(true, Ordering::Release);
    } else {
        stage("kvm_run", memory_bytes);
    }
    *gate.0.lock().unwrap() = true;
    gate.1.notify_all();

    let mut outcome = failure.map(|error| Outcome::Failed("kvm_error", error));
    while outcome.is_none() || workers.iter().any(|worker| !worker.is_finished()) {
        if outcome.is_none() {
            if let Ok(decision) = receiver.try_recv() {
                outcome = Some(decision);
            } else if cancelled().is_some() {
                outcome = Some(Outcome::HostStop);
            } else if SAVE_SIGNAL.swap(false, Ordering::AcqRel) {
                outcome = Some(Outcome::Save);
            } else if workers.iter().any(|worker| worker.is_finished()) {
                outcome = Some(receiver.try_recv().unwrap_or_else(|_| {
                    Outcome::Failed(
                        "kvm_error",
                        anyhow::anyhow!("a vCPU stopped without an outcome"),
                    )
                }));
            }
        }
        if outcome.is_some() {
            control.stopped.store(true, Ordering::Release);
            for worker in &workers {
                if !worker.is_finished() {
                    // A signal may arrive immediately before KVM_RUN. Retrying
                    // until the thread exits also covers that entry window.
                    unsafe {
                        libc::pthread_kill(worker.as_pthread_t(), libc::SIGUSR1);
                    }
                }
            }
        }
        unsafe {
            libc::poll(std::ptr::null_mut(), 0, 5);
        }
    }
    let mut cpus = Vec::with_capacity(count);
    let mut total = Stats::default();
    for worker in workers {
        let (cpu, stats) = worker
            .join()
            .map_err(|_| anyhow::anyhow!("vCPU worker panicked"))?;
        cpus.push(cpu);
        total.merge(stats);
    }
    Ok((outcome.unwrap(), cpus, total))
}

fn run_cpu(
    vcpu: &mut VcpuFd,
    vm: &VmFd,
    memory: &Memory,
    devices: &Devices,
    control: &Control,
    stats: &mut Stats,
) -> Result<Option<Outcome>> {
    while !control.stopped.load(Ordering::Acquire) {
        let event = match vcpu.run() {
            Ok(event) => event,
            Err(error) if error.errno() == libc::EINTR || error.errno() == libc::EAGAIN => continue,
            Err(error) => return Err(error).context("KVM_RUN"),
        };
        stats.record(&event);
        let mut candidate = None;
        match event {
            VcpuExit::IoOut(port, data) => {
                if matches!(port, 0x604..=0x607) && control.workload && data.len() <= 4 {
                    let mut value = [0; 4];
                    value[..data.len()].copy_from_slice(data);
                    candidate = Some((port, value, data.len()));
                } else if let Some(&byte) = data.first() {
                    match port {
                        0x600 if byte & 0x3c == 0x34 => {
                            return Ok(Some(Outcome::Guest("poweroff")));
                        }
                        0xcf9 if byte & 4 != 0 => return Ok(Some(Outcome::Guest("reset"))),
                        0x64 if byte == 0xfe => return Ok(Some(Outcome::Guest("reset"))),
                        _ => {
                            if devices.io_write(port, data)? {
                                stats.serial += 1;
                            }
                        }
                    }
                }
            }
            VcpuExit::IoIn(port, data) => {
                if matches!(port, 0x600 | 0x601 | 0xcf9) {
                    data.fill(0);
                } else {
                    let payload: *mut [u8] = data;
                    let width = unsafe { vcpu.get_kvm_run().__bindgen_anon_1.io.size } as usize;
                    ensure!(matches!(width, 1 | 2 | 4), "invalid I/O width {width}");
                    // The PIO data page is disjoint from the kvm_run header,
                    // and this thread owns both until the next KVM_RUN.
                    let data = unsafe { &mut *payload };
                    if devices.io_read(port, width, data)? {
                        stats.serial += 1;
                    } else {
                        data.fill(0xff);
                    }
                }
            }
            VcpuExit::MmioRead(address, data) => {
                if !devices.mmio(address, data, false)? {
                    data.fill(0xff);
                }
            }
            VcpuExit::MmioWrite(address, data) => {
                let mut buffer = [0; 8];
                ensure!(
                    data.len() <= buffer.len(),
                    "unsupported KVM MMIO write length"
                );
                buffer[..data.len()].copy_from_slice(data);
                devices.mmio(address, &mut buffer[..data.len()], true)?;
            }
            VcpuExit::Hypercall(call) if control.tdx && call.nr == 12 => {
                match memory.convert(vm, call.args[0], call.args[1], call.args[2]) {
                    Ok(()) => *call.ret = 0,
                    Err(error) if error.is::<InvalidMapGpa>() => {
                        *call.ret = -(libc::EINVAL as i64) as u64
                    }
                    Err(error) => return Err(error),
                }
            }
            VcpuExit::Hlt => return Ok(Some(Outcome::Guest("hlt"))),
            VcpuExit::Shutdown => {
                return Ok(Some(Outcome::Failed(
                    "shutdown",
                    anyhow::anyhow!("guest shutdown (possible triple fault)"),
                )));
            }
            VcpuExit::SystemEvent(kind, _) if control.tdx => {
                return Ok(Some(Outcome::Failed(
                    "tdx_system_event",
                    anyhow::anyhow!("TDX guest system event {kind}"),
                )));
            }
            VcpuExit::Hypercall(_) => {
                return Ok(Some(Outcome::Failed(
                    "unhandled_hypercall",
                    anyhow::anyhow!("guest made an unsupported hypercall"),
                )));
            }
            unexpected => {
                return Ok(Some(Outcome::Failed(
                    "unexpected_exit",
                    anyhow::anyhow!("unexpected KVM exit: {unexpected:?}"),
                )));
            }
        }
        if let Some((port, data, len)) = candidate {
            // The safe ioctl wrapper omits the count/width metadata from
            // IoOut. The borrowed data has been copied and is no longer live.
            let io = unsafe { vcpu.get_kvm_run().__bindgen_anon_1.io };
            if io.count == 1 && io.size as usize == len {
                if let Some(code) = completion(control, port, &data[..len]) {
                    eprintln!("INFO: workload exited status={code}");
                    return Ok(Some(Outcome::Workload(code)));
                }
            }
        }
    }
    Ok(None)
}

fn completion(control: &Control, port: u16, data: &[u8]) -> Option<i32> {
    if !control.tdx {
        if port != 0x604 || data.len() != 4 {
            return None;
        }
        let word = u32::from_le_bytes(data.try_into().ok()?);
        return (word & 0xffff_ff00 == 0x5356_4d00).then_some((word & 255) as i32);
    }
    if data.len() != 1 {
        return None;
    }
    let index = usize::from(port - 0x604);
    let mut frame = control.frame.lock().unwrap();
    if index == 0 {
        *frame = 0;
    }
    if index != *frame || index < 3 && data[0] != b"SVM"[index] {
        *frame = 0;
        return None;
    }
    if index == 3 {
        *frame = 0;
        Some(i32::from(data[0]))
    } else {
        *frame += 1;
        None
    }
}

struct Console {
    wake: File,
    worker: Option<JoinHandle<()>>,
    termios: Option<libc::termios>,
    devices: Arc<Devices>,
}

impl Console {
    fn start(devices: Arc<Devices>, save_enabled: bool) -> Result<Self> {
        let wake = unsafe { libc::eventfd(0, libc::EFD_CLOEXEC | libc::EFD_NONBLOCK) };
        if wake < 0 {
            return Err(io::Error::last_os_error()).context("create console stop event");
        }
        let wake = unsafe { File::from_raw_fd(wake) };
        let termios = if unsafe { libc::isatty(0) } == 1 {
            let mut original = std::mem::MaybeUninit::<libc::termios>::uninit();
            if unsafe { libc::tcgetattr(0, original.as_mut_ptr()) } != 0 {
                return Err(io::Error::last_os_error()).context("read terminal settings");
            }
            let original = unsafe { original.assume_init() };
            let mut raw = original;
            unsafe {
                libc::cfmakeraw(&mut raw);
            }
            if unsafe { libc::tcsetattr(0, libc::TCSANOW, &raw) } != 0 {
                return Err(io::Error::last_os_error()).context("configure terminal");
            }
            Some(original)
        } else {
            None
        };
        let mut console = Self {
            wake,
            worker: None,
            termios,
            devices,
        };
        let fd = console.wake.as_raw_fd();
        let device = console.devices.clone();
        let raw = console.termios.is_some();
        console.worker = Some(thread::Builder::new().name("svmm-console".into()).spawn(
            move || {
                console_loop(&device, fd, raw, save_enabled);
            },
        )?);
        Ok(console)
    }
}

impl Drop for Console {
    fn drop(&mut self) {
        self.devices.close_input();
        let _ = (&self.wake).write_all(&1u64.to_ne_bytes());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        if let Some(termios) = &self.termios {
            unsafe {
                libc::tcsetattr(0, libc::TCSANOW, termios);
            }
        }
    }
}

fn console_loop(devices: &Devices, wake: i32, raw: bool, save_enabled: bool) {
    let mut fds = [
        libc::pollfd {
            fd: 0,
            events: libc::POLLIN,
            revents: 0,
        },
        libc::pollfd {
            fd: wake,
            events: libc::POLLIN,
            revents: 0,
        },
    ];
    let mut data = [0u8; 256];
    let mut escaped = false;
    loop {
        let ready = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, -1) };
        if ready < 0 && io::Error::last_os_error().kind() == io::ErrorKind::Interrupted {
            continue;
        }
        if ready <= 0 || fds[1].revents != 0 {
            return;
        }
        let read = unsafe { libc::read(0, data.as_mut_ptr().cast(), data.len()) };
        if read < 0 && io::Error::last_os_error().kind() == io::ErrorKind::Interrupted {
            continue;
        }
        if read <= 0 {
            return;
        }
        let mut output = Vec::with_capacity(read as usize);
        let mut quit = false;
        let mut save = false;
        for &byte in &data[..read as usize] {
            if !raw {
                output.push(byte);
            } else if escaped {
                escaped = false;
                match byte {
                    b'x' | b'X' => {
                        quit = true;
                        break;
                    }
                    b's' | b'S' => save = true,
                    1 => output.push(1),
                    _ => {}
                }
            } else if byte == 1 {
                escaped = true;
            } else {
                output.push(byte);
            }
        }
        if !output.is_empty() && devices.serial_input(&output).is_err() {
            return;
        }
        if save {
            if save_enabled {
                SAVE_SIGNAL.store(true, Ordering::Release);
            } else {
                eprintln!("\r\nINFO: Ctrl-A s ignored: no --warm-save NAME given\r");
            }
        }
        if quit {
            crate::request_stop(libc::SIGTERM);
            return;
        }
    }
}

use anyhow::{Result, bail, ensure};
use std::path::PathBuf;

use crate::{jail::Profile, oci::ImageOptions, snapshot::Info};

pub const HELP: &str = "\
Usage: svmm run --kernel PATH [options]
       svmm run --image IMAGE [options] [-- COMMAND ARGS...]
       svmm run --restore NAME [--disk PATH] [options]
       svmm snapshot list|save|validate|delete [options]

  -k, --kernel PATH   x86 bzImage (auto-selected with --image)
  -i, --initrd PATH   initramfs
  -m, --mem SIZE_MB   guest memory in MiB (default 256)
  -c, --cmdline STR   kernel command line
  -d, --disk PATH     virtio-blk disk image
  -t, --tap IFNAME    existing TAP interface for virtio-net
  -n, --netns NAME    host network namespace containing the TAP
  -j, --jail          restrict the VMM using cgroups, rlimits and seccomp
      --cpu-max USEC  CPU bandwidth limit with --jail
      --memory-max MB memory limit with --jail
      --nofile N      descriptor limit with --jail
      --nproc N       task limit with --jail
      --tdx           native KVM Intel TDX
      --td-shim PATH  TD-Shim firmware
      --vcpus N       vCPUs with --tdx (1–64, default 1)
      --workload      run an OCI workload from an existing disk
      --image IMAGE   prepare and run an OCI image
      --entrypoint EXE  override image entrypoint
      --env KEY=VALUE   set an environment variable (repeatable)
      --workdir PATH    set an absolute working directory
      --user USER       set the workload identity
      --size-mb N      set the OCI disk size
  -s, --snapshot NAME  restore a cold disk snapshot
      --warm-save NAME save a warm snapshot on Ctrl-A s or SIGUSR2
      --restore NAME   restore a warm snapshot
  -h, --help          print this help

Press Ctrl-A x to stop the VMM.
";

const SNAPSHOT_HELP: &str = "\
Usage: svmm snapshot list
       svmm snapshot save --name NAME --disk PATH [--kernel PATH] [--initrd PATH] [--mem MB] [--cmdline STR]
       svmm snapshot validate|delete --name NAME
";

pub const CMDLINE: &str = "console=ttyS0 earlycon=uart8250,io,0x3f8 init=/init loglevel=8 acpi_rsdp=0xe0000 reboot=acpi panic=-1";
pub const TD_CMDLINE: &str =
    "console=ttyS0 earlycon=uart8250,io,0x3f8 loglevel=7 reboot=acpi panic=1 nokaslr";

pub enum Action {
    Help(&'static str),
    Run(Box<Run>),
    Build(ImageOptions, Vec<String>, PathBuf),
    Snapshot(Snapshot),
}

pub enum Snapshot {
    List,
    Save(String, PathBuf, Info),
    Validate(String),
    Delete(String),
}

pub struct Run {
    pub kernel: Option<PathBuf>,
    pub initrd: Option<PathBuf>,
    pub disk: Option<PathBuf>,
    pub tap: Option<String>,
    pub netns: Option<String>,
    pub mem: usize,
    pub cmdline: String,
    pub vcpus: u32,
    pub tdx: bool,
    pub firmware: Option<PathBuf>,
    pub workload: bool,
    pub image: Option<ImageOptions>,
    pub command: Vec<String>,
    pub jail: Option<Profile>,
    pub cold: Option<String>,
    pub save: Option<String>,
    pub restore: Option<String>,
}

struct Arg<'a> {
    name: &'a str,
    value: Option<&'a str>,
}

fn short(name: u8) -> Result<(&'static str, bool)> {
    Ok(match name {
        b'k' => ("kernel", true),
        b'i' => ("initrd", true),
        b'd' => ("disk", true),
        b'm' => ("mem", true),
        b'c' => ("cmdline", true),
        b't' => ("tap", true),
        b'n' => ("netns", true),
        b's' => ("snapshot", true),
        b'j' => ("jail", false),
        b'h' => ("help", false),
        _ => bail!("unknown option '-{}'", char::from(name)),
    })
}

fn arguments(args: &[String]) -> Result<(Vec<Arg<'_>>, &[String])> {
    parse_arguments(args, false)
}

fn parse_arguments(args: &[String], allow_remainder: bool) -> Result<(Vec<Arg<'_>>, &[String])> {
    let mut output = Vec::new();
    let mut index = 0;
    while let Some(arg) = args.get(index) {
        index += 1;
        if arg == "--" {
            return Ok((output, &args[index..]));
        }
        if let Some(option) = arg.strip_prefix("--") {
            let (name, attached) = option
                .split_once('=')
                .map_or((option, None), |(name, value)| (name, Some(value)));
            let value = match name {
                "help" | "jail" | "tdx" | "workload" => {
                    ensure!(attached.is_none(), "--{name} takes no argument");
                    None
                }
                "kernel" | "initrd" | "disk" | "mem" | "cmdline" | "tap" | "netns" | "td-shim"
                | "vcpus" | "image" | "entrypoint" | "env" | "workdir" | "user" | "size-mb"
                | "output" | "name" | "snapshot" | "warm-save" | "restore" | "cpu-max"
                | "memory-max" | "nofile" | "nproc" => {
                    let value = match attached {
                        Some(value) => value,
                        None => {
                            let value = args.get(index).ok_or_else(|| {
                                anyhow::anyhow!("option '--{name}' requires an argument")
                            })?;
                            index += 1;
                            value
                        }
                    };
                    Some(value)
                }
                _ => bail!("unknown option '--{name}'"),
            };
            output.push(Arg { name, value });
        } else if let Some(group) = arg.strip_prefix('-').filter(|s| !s.is_empty()) {
            for (offset, byte) in group.bytes().enumerate() {
                let (name, consumes) = short(byte)?;
                let value = if consumes {
                    let tail = &group[offset + 1..];
                    if tail.is_empty() {
                        let value = args.get(index).ok_or_else(|| {
                            anyhow::anyhow!("option '-{}' requires an argument", char::from(byte))
                        })?;
                        index += 1;
                        Some(value.as_str())
                    } else {
                        Some(tail)
                    }
                } else {
                    None
                };
                output.push(Arg { name, value });
                if consumes {
                    break;
                }
            }
        } else if allow_remainder {
            return Ok((output, &args[index - 1..]));
        } else {
            bail!("unexpected argument '{arg}'");
        }
    }
    Ok((output, &[]))
}

fn nonempty<'a>(name: &str, value: &'a str) -> Result<&'a str> {
    ensure!(!value.is_empty(), "--{name} requires a non-empty value");
    Ok(value)
}

fn number(name: &str, value: &str, min: u64, max: u64) -> Result<u64> {
    let result = value
        .parse::<u64>()
        .ok()
        .filter(|value| (min..=max).contains(value));
    ensure!(
        value.bytes().all(|c| c.is_ascii_digit()) && result.is_some(),
        "--{name} must be an integer from {min} to {max}, got '{value}'"
    );
    Ok(result.unwrap())
}

pub fn identifier(value: &str, max: usize, dots: bool) -> bool {
    !value.is_empty()
        && value.len() <= max
        && value != "."
        && value != ".."
        && value
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_' || dots && c == b'.')
}

fn snapshot_name(name: &str) -> Result<String> {
    ensure!(
        identifier(name, 128, false),
        "invalid snapshot name '{name}'"
    );
    Ok(name.to_owned())
}

fn image_option(image: &mut ImageOptions, name: &str, value: &str) -> Result<bool> {
    match name {
        "image" => image.image = nonempty(name, value)?.into(),
        "entrypoint" => image.entrypoint = Some(value.into()),
        "env" => {
            ensure!(
                value
                    .split_once('=')
                    .is_some_and(|(key, _)| !key.is_empty()),
                "--env requires KEY=VALUE"
            );
            image.env.push(value.into());
        }
        "workdir" => {
            ensure!(
                value.starts_with('/'),
                "--workdir requires an absolute path"
            );
            image.workdir = Some(value.into());
        }
        "user" => image.user = Some(nonempty(name, value)?.into()),
        "size-mb" => image.size_mb = Some(number(name, value, 16, 1 << 20)?),
        _ => return Ok(false),
    }
    Ok(true)
}

const IMAGE_BUILD_HELP: &str = "\
Usage: svmm image-build --image IMAGE --output PATH [options] [-- COMMAND ARGS...]

Export an OCI image to a writable SVMM disk.

      --image IMAGE         linux/amd64 OCI image reference
      --output PATH         destination ext4 disk
      --entrypoint EXE      override the image entrypoint and clear its default command
      --env KEY=VALUE       set an environment variable (repeatable)
      --workdir PATH        set an absolute working directory
      --user USER[:GROUP]   set the workload identity
      --size-mb N           set the disk size in MiB (minimum 16)
  -h, --help                print this help
";

pub fn parse(args: &[String]) -> Result<Action> {
    let (first, tail) = args
        .split_first()
        .ok_or_else(|| anyhow::anyhow!("missing subcommand"))?;
    match first.as_str() {
        "help" | "--help" | "-h" => Ok(Action::Help(HELP)),
        "run" => run(tail),
        "snapshot" => snapshot(tail),
        "image-build" => {
            let (options, command) = parse_arguments(tail, true)?;
            if options.iter().any(|arg| arg.name == "help") {
                return Ok(Action::Help(IMAGE_BUILD_HELP));
            }
            let mut image = image_defaults();
            let mut output = None;
            for arg in options {
                let value = arg.value.unwrap_or("");
                if arg.name == "output" {
                    output = Some(PathBuf::from(nonempty("output", value)?));
                } else {
                    ensure!(
                        image_option(&mut image, arg.name, value)?,
                        "invalid image-build option --{}",
                        arg.name
                    );
                }
            }
            ensure!(!image.image.is_empty(), "--image is required");
            Ok(Action::Build(
                image,
                command.to_owned(),
                output.ok_or_else(|| anyhow::anyhow!("--output is required"))?,
            ))
        }
        _ => bail!("unknown subcommand '{first}'"),
    }
}

fn image_defaults() -> ImageOptions {
    ImageOptions {
        image: String::new(),
        entrypoint: None,
        env: Vec::new(),
        user: None,
        workdir: None,
        size_mb: None,
    }
}

fn run(args: &[String]) -> Result<Action> {
    let (options, command) = arguments(args)?;
    let mut config = Run {
        kernel: None,
        initrd: None,
        disk: None,
        tap: None,
        netns: None,
        mem: 256,
        cmdline: String::new(),
        vcpus: 1,
        tdx: false,
        firmware: None,
        workload: false,
        image: None,
        command: command.to_owned(),
        jail: None,
        cold: None,
        save: None,
        restore: None,
    };
    let mut image = image_defaults();
    let mut image_args = false;
    let mut vcpus_specified = false;
    let mut memory_values = Vec::new();
    let mut boot_option = false;
    let mut command_line = None;
    let mut jail_enabled = false;
    let mut limits = Profile::default();
    let mut jail_args = false;
    for arg in options {
        let value = arg.value.unwrap_or("");
        match arg.name {
            "help" => return Ok(Action::Help(HELP)),
            "kernel" => {
                config.kernel = Some(value.into());
                boot_option = true;
            }
            "initrd" => {
                config.initrd = (!value.is_empty()).then(|| value.into());
                boot_option = true;
            }
            "mem" => {
                memory_values.push(value);
                boot_option = true;
            }
            "cmdline" => {
                command_line = Some(value);
                boot_option = true;
            }
            "disk" => config.disk = Some(nonempty("disk", value)?.into()),
            "tap" => config.tap = Some(nonempty("tap", value)?.into()),
            "netns" => config.netns = Some(nonempty("netns", value)?.into()),
            "tdx" => config.tdx = true,
            "td-shim" => config.firmware = Some(nonempty("td-shim", value)?.into()),
            "vcpus" => {
                config.vcpus = number("vcpus", value, 1, 64)? as u32;
                vcpus_specified = true;
            }
            "workload" => config.workload = true,
            "jail" => jail_enabled = true,
            "cpu-max" => {
                limits.cpu_max = number("cpu-max", value, 1000, 100_000_000)?;
                jail_args = true;
            }
            "memory-max" => {
                limits.memory = number("memory-max", value, 1, 1 << 20)?;
                jail_args = true;
            }
            "nofile" => {
                limits.nofile = number("nofile", value, 1, 1 << 20)?;
                jail_args = true;
            }
            "nproc" => {
                limits.nproc = number("nproc", value, 1, 1 << 20)?;
                jail_args = true;
            }
            "snapshot" => {
                config.cold = Some(snapshot_name(value)?);
                boot_option = true;
            }
            "warm-save" => config.save = Some(snapshot_name(value)?),
            "restore" => config.restore = Some(snapshot_name(value)?),
            name => {
                ensure!(
                    image_option(&mut image, name, value)?,
                    "invalid run option --{name}"
                );
                image_args |= name != "image";
            }
        }
    }
    for value in memory_values {
        config.mem = number("mem", value, 64, if config.tdx { 32768 } else { 3072 })? as usize;
    }
    ensure!(
        !image.image.is_empty() || !image_args && command.is_empty(),
        "--entrypoint/--env/--workdir/--user/--size-mb and application arguments require --image"
    );
    if !image.image.is_empty() {
        ensure!(
            config.disk.is_none() && config.cold.is_none() && config.restore.is_none(),
            "--image cannot be combined with --disk, --snapshot or --restore"
        );
        config.image = Some(image);
    }
    ensure!(
        config.tdx || config.firmware.is_none(),
        "--td-shim requires --tdx"
    );
    ensure!(config.tdx || !vcpus_specified, "--vcpus requires --tdx");
    ensure!(
        !config.tdx
            || !jail_enabled
                && config.cold.is_none()
                && config.save.is_none()
                && config.restore.is_none(),
        "--tdx does not support jail or snapshots"
    );
    ensure!(
        config.restore.is_none() || !boot_option,
        "--restore takes the kernel, memory size and command line from the snapshot"
    );
    ensure!(
        config.image.is_some()
            || config.restore.is_some()
            || config
                .kernel
                .as_ref()
                .is_some_and(|p| !p.as_os_str().is_empty()),
        "--kernel is required"
    );
    ensure!(
        !jail_enabled || config.save.is_none() && config.restore.is_none(),
        "--warm-save/--restore cannot be combined with --jail"
    );
    ensure!(
        config.cold.is_none() || config.disk.is_some(),
        "--snapshot requires --disk"
    );
    ensure!(
        !jail_args || jail_enabled,
        "jail limit options require --jail"
    );
    ensure!(
        !config.workload
            || config.image.is_some()
            || config.disk.is_some() && (config.initrd.is_some() || config.restore.is_some()),
        "--workload requires --disk and, for cold boot, --initrd"
    );
    config.cmdline = command_line
        .unwrap_or(if config.tdx { TD_CMDLINE } else { CMDLINE })
        .into();
    if jail_enabled {
        limits.finish(config.mem);
        config.jail = Some(limits);
    }
    Ok(Action::Run(Box::new(config)))
}

fn snapshot(args: &[String]) -> Result<Action> {
    let (action, tail) = args
        .split_first()
        .ok_or_else(|| anyhow::anyhow!("missing snapshot action"))?;
    if matches!(action.as_str(), "-h" | "--help" | "help") {
        return Ok(Action::Help(SNAPSHOT_HELP));
    }
    ensure!(
        matches!(action.as_str(), "list" | "save" | "validate" | "delete"),
        "unknown snapshot action '{action}'"
    );
    let (args, command) = arguments(tail)?;
    ensure!(command.is_empty(), "unexpected snapshot argument");
    let mut name = None;
    let mut disk = None;
    let mut info = Info::default();
    for arg in args {
        if arg.name == "help" {
            return Ok(Action::Help(SNAPSHOT_HELP));
        }
        ensure!(
            action == "save" || action != "list" && arg.name == "name",
            "option '--{}' is not valid for 'snapshot {action}'",
            arg.name
        );
        let value = arg.value.unwrap_or("");
        match arg.name {
            "name" => name = Some(snapshot_name(value)?),
            "disk" => disk = Some(PathBuf::from(nonempty("disk", value)?)),
            "kernel" => info.kernel = Some(value.into()),
            "initrd" => info.initrd = Some(value.into()),
            "cmdline" => info.cmdline = Some(value.into()),
            "mem" => info.mem = Some(number("mem", value, 64, 3072)? as usize),
            _ => bail!("invalid snapshot option --{}", arg.name),
        }
    }
    if action == "list" {
        return Ok(Action::Snapshot(Snapshot::List));
    }
    let name = name.ok_or_else(|| anyhow::anyhow!("'snapshot {action}' requires --name"))?;
    Ok(Action::Snapshot(match action.as_str() {
        "save" => Snapshot::Save(
            name,
            disk.ok_or_else(|| anyhow::anyhow!("'snapshot save' requires --disk"))?,
            info,
        ),
        "delete" => Snapshot::Delete(name),
        _ => Snapshot::Validate(name),
    }))
}

# SVMM

**English** · [简体中文](README.zh-CN.md)

**Run AI tasks and OCI workloads in local KVM microVMs.**

SVMM is a Rust virtual machine monitor for Linux x86_64. Give it an OCI image to run a script, build, test, or other short-lived job in an isolated Linux guest. SVMM returns the application's output and exit status.

[Quick start](#quick-start) · [CLI usage](#cli-usage) · [AI coding assistants](#ai-coding-assistants) · [Intel TDX](#intel-tdx) · [Detailed guide (Chinese)](docs/guide.md)

## Features

| Feature | What it supports |
| --- | --- |
| OCI images | Use a cached Docker image or pull a missing one, prepare a writable ext4 root filesystem, and launch the app with its image configuration |
| One-shot sandboxes | Run a command or script, return its exit status, and clean up the temporary disk |
| Resource limits and networking | Constrain the VMM with the jailer on standard KVM and connect the guest to a host TAP interface and network namespace as needed |
| Disks and snapshots | Keep artifacts on a virtio-blk disk, or save and restore cold and warm snapshots on standard KVM |
| Intel TDX | Boot with native KVM TDX, up to 64 vCPUs and 32 GiB of RAM, with virtio-blk and TAP support |

## Quick start

Use a Linux x86_64 host with access to `/dev/kvm`, Rust 1.85 or later, and a linker. Running OCI images also requires the Docker CLI and daemon, `e2fsprogs`, `cpio`, `gzip`, a compatible guest bzImage, and root privileges. The guest kernel needs built-in drivers such as virtio-mmio, virtio-blk, and ext4. [Kernel and image configuration (Chinese)](docs/guide.md#stage-12直接运行-oci--docker-镜像里的应用)

```sh
git clone https://github.com/billionairiam/SVMM.git
cd SVMM
rustup target add x86_64-unknown-linux-musl
make all workload-initramfs
```

Boot an Ubuntu guest, run a command, and exit.

```sh
sudo bin/svmm run --image ubuntu:24.04 --kernel /path/to/bzImage \
  --entrypoint /bin/sh -- -c 'echo hello-from-svmm'
```

With a default kernel configured, run the image's own ENTRYPOINT and CMD.

```sh
sudo bin/svmm run --image my-app:latest
```

Set the guest kernel with `--kernel` or `SVMM_KERNEL`. The `run --image` command can also look for a compatible kernel in the repository's `images/` directory and the host's `/boot/` directory. The first run may need to pull the image.

## CLI usage

### Run a script or test with a time limit

Add `--jail` on standard KVM. Use `--mem` for guest RAM and `--cpu-max` and `--memory-max` for the host cgroup limits.

```sh
sudo timeout --signal=TERM --kill-after=10s 90s bin/svmm run \
  --image ubuntu:24.04 --kernel /path/to/bzImage \
  --mem 256 --jail --cpu-max 50000 --memory-max 384 \
  --entrypoint /bin/sh -- -c 'echo task-finished; exit 7'
echo "$?"    # 7
```

The guest's stdout and stderr share the serial stream. The VMM writes its diagnostics separately to host stderr, including `reason=workload_exit` when the app reports completion. Guests boot without a NIC by default, and Docker prepares the image on the host. For networking, pass `--tap tap0 --netns sandbox-ns` with an existing host setup. [TAP and egress policy (Chinese)](docs/guide.md#stage-08网络命名空间与出口策略)

### Keep task artifacts

SVMM cleans up one-shot sandbox disks when the VM exits. To inspect or reuse an artifact, create a separate ext4 work disk and boot with `--disk`.

```sh
sudo bin/svmm image-build --image ubuntu:24.04 --output build/task.ext4 \
  --entrypoint /bin/sh -- -c 'echo finished > /tmp/task-result'

sudo bin/svmm run --kernel /path/to/bzImage \
  --initrd build/workload-initramfs.cpio.gz --disk build/task.ext4 --workload

sudo debugfs -R 'cat /tmp/task-result' build/task.ext4
```

### Pause and resume

A cold snapshot saves the work disk for a fresh guest boot. On standard KVM, a warm snapshot saves memory, vCPU state, device state, and the disk so the guest can resume on the same host. [Snapshot guide (Chinese)](docs/guide.md#stage-11热快照从暂停点毫秒级恢复)

```sh
sudo bin/svmm run --image my-app:latest --kernel /path/to/bzImage --warm-save checkpoint
# While the workload is running, press Ctrl-A s to save and exit.
sudo bin/svmm run --restore checkpoint --disk build/resumed.ext4 --workload
```

## AI coding assistants

Open Codex in the repository and use [`$svmm-sandbox`](.agents/skills/svmm-sandbox/SKILL.md) to run a task in a separate VM. Other coding assistants can read the same `SKILL.md` file directly.

> Use `$svmm-sandbox` to run this script in an Ubuntu 24.04 VM with guest networking disabled. Give it 90 seconds and return the application output and exit status.

The skill covers image selection, script inputs, time limits, and artifact retrieval. For SVMM development, see the [E2E](.agents/skills/svmm-e2e/SKILL.md), [OCI](.agents/skills/svmm-oci/SKILL.md), [TDX](.agents/skills/svmm-tdx/SKILL.md), and [snapshot](.agents/skills/svmm-snapshots/SKILL.md) skills.

## Intel TDX

On an Intel host with KVM TDX enabled, use TD-Shim and a TDX guest kernel to boot a Trust Domain. This example allocates 4 vCPUs.

```sh
sudo bin/svmm run --tdx --vcpus 4 \
  --td-shim /path/to/td-shim.bin --kernel /path/to/tdx-bzImage \
  --image ubuntu:24.04 --entrypoint /bin/sh -- -c 'echo hello-from-tdx'
```

The vCPU limit depends on the host KVM, TD-Shim, and guest kernel configuration. TDX protects private guest memory. The host can still access the rootfs file, serial output, and TAP traffic. Hardware tests verify boot with a non-debug TDREPORT. Remote attestation requires a Quote and a verifier. The jailer and snapshots are available on standard KVM. [TDX firmware, kernel configuration, and limits (Chinese)](docs/guide.md#stage-13在-intel-tdx-中运行-oci-镜像)

## Development and testing

The runtime source is in [`src-rust/`](src-rust/). Start with the E2E cases affected by your change and use `make test` for the full standard KVM suite. [Testing conventions (Chinese)](AGENTS.md)

```sh
sudo env VIRTIO_BZIMAGE=/path/to/bzImage make test-workload
sudo env SVMM_TEST_TDX_KERNEL=/path/to/tdx-bzImage \
  SVMM_TEST_TD_SHIM=/path/to/td-shim.bin make test-tdx
```

The [detailed guide (Chinese)](docs/guide.md) covers kernel boot, virtio, networking, snapshots, and ablation experiments. The educational C implementation is in [`stage11`](https://github.com/billionairiam/SVMM/tree/stage11).

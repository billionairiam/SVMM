# SVMM

**用本地 KVM 微虚拟机运行 AI 任务和 OCI workload。**

SVMM 是一个面向 Linux x86_64 的 Rust VMM。指定一个 OCI 镜像，就能在独立的 Linux guest 中执行脚本、构建、测试或短任务，并拿到应用的输出和退出码。

[快速开始](#快速开始) · [CLI 用法](#cli-用法) · [AI 编程助手](#ai-编程助手) · [Intel TDX](#intel-tdx) · [使用与实现指南](docs/guide.md)

## 主要能力

| 能力 | 支持范围 |
| --- | --- |
| OCI 镜像 | 复用本地 Docker 镜像或拉取缺失镜像，生成可写的 ext4 rootfs，按镜像配置启动应用 |
| 一次性沙箱 | 以命令或脚本为入口，应用退出后回传状态并清理临时磁盘 |
| 资源与网络 | 普通 KVM 可用 jailer 约束 VMM，guest 网络按需接入宿主 TAP 和 netns |
| 磁盘与快照 | 可通过 virtio-blk 持久化产物，普通 KVM 支持冷恢复和热续跑 |
| Intel TDX | 原生 KVM TDX 启动，最多 64 vCPU 和 32 GiB 内存，支持 virtio-blk 与 TAP |

## 快速开始

宿主机需要 Linux x86_64、可访问的 `/dev/kvm`、Rust 1.85+ 和链接工具。OCI 路径还需要 Docker CLI/daemon、`e2fsprogs`、`cpio`、`gzip`、兼容的 guest bzImage 和 root 权限。guest 内核需内建 virtio-mmio、virtio-blk、ext4 等启动驱动。 [内核与镜像配置](docs/guide.md#stage-12直接运行-oci--docker-镜像里的应用)

```sh
git clone https://github.com/billionairiam/SVMM.git
cd SVMM
rustup target add x86_64-unknown-linux-musl
make all workload-initramfs
```

用 Ubuntu 镜像启动一个 guest，执行命令后退出。

```sh
sudo bin/svmm run --image ubuntu:24.04 --kernel /path/to/bzImage \
  --entrypoint /bin/sh -- -c 'echo hello-from-svmm'
```

如果已经配置默认内核，直接运行镜像自带的 ENTRYPOINT 和 CMD。

```sh
sudo bin/svmm run --image my-app:latest
```

内核可通过 `--kernel` 或 `SVMM_KERNEL` 指定。`run --image` 也会从仓库的 `images/` 和宿主的 `/boot/` 查找兼容内核。首次运行可能需要拉取镜像。

## CLI 用法

### 限时运行脚本或测试

普通 KVM 可加 `--jail`。`--mem` 配置 guest RAM，`--cpu-max` 和 `--memory-max` 配置宿主 cgroup 限制。

```sh
sudo timeout --signal=TERM --kill-after=10s 90s bin/svmm run \
  --image ubuntu:24.04 --kernel /path/to/bzImage \
  --mem 256 --jail --cpu-max 50000 --memory-max 384 \
  --entrypoint /bin/sh -- -c 'echo task-finished; exit 7'
echo "$?"    # 7
```

应用的 stdout 和 stderr 经串口合流，VMM 诊断单独写到宿主 stderr。正常应用退出时可看到 `reason=workload_exit`。未配置 `--tap` 时 guest 无网卡，Docker 的镜像准备仍由宿主执行。联网任务可传 `--tap tap0 --netns sandbox-ns`，并复用宿主配置。 [TAP 与出口策略](docs/guide.md#stage-08网络命名空间与出口策略)

### 保留任务产物

一次性沙箱的磁盘随 VM 退出清理。需要读取或复用产物时，先创建独立的 ext4 工作盘，再用 `--disk` 运行。

```sh
sudo bin/svmm image-build --image ubuntu:24.04 --output build/task.ext4 \
  --entrypoint /bin/sh -- -c 'echo finished > /tmp/task-result'

sudo bin/svmm run --kernel /path/to/bzImage \
  --initrd build/workload-initramfs.cpio.gz --disk build/task.ext4 --workload

sudo debugfs -R 'cat /tmp/task-result' build/task.ext4
```

### 暂停与恢复

冷快照保存工作盘，恢复后重新启动 guest。热快照保存普通 KVM 的内存、vCPU、设备和磁盘状态，在同一宿主上从暂停点继续。 [快照操作](docs/guide.md#stage-11热快照从暂停点毫秒级恢复)

```sh
sudo bin/svmm run --image my-app:latest --kernel /path/to/bzImage --warm-save checkpoint
# 在 workload 运行期间按 Ctrl-A s，保存并退出。
sudo bin/svmm run --restore checkpoint --disk build/resumed.ext4 --workload
```

## AI 编程助手

在仓库中打开 Codex，使用 [`$svmm-sandbox`](.agents/skills/svmm-sandbox/SKILL.md) 把一次任务交给独立 VM。其他编程助手可直接读取对应的 `SKILL.md`。

> 用 `$svmm-sandbox` 在 Ubuntu 24.04 VM 里运行这个脚本，关闭 guest 网络，最多运行 90 秒，返回应用输出和退出码。

这个技能说明如何选镜像、传入脚本、限制运行时间，以及从工作盘取回产物。开发 SVMM 时另有 [E2E](.agents/skills/svmm-e2e/SKILL.md)、[OCI](.agents/skills/svmm-oci/SKILL.md)、[TDX](.agents/skills/svmm-tdx/SKILL.md) 和 [快照](.agents/skills/svmm-snapshots/SKILL.md) 技能。

## Intel TDX

在启用 KVM TDX 的 Intel 宿主上，用 TD-Shim 和 TDX guest 内核启动 Trust Domain。下面的命令为同一个 workload 分配 4 个 vCPU。

```sh
sudo bin/svmm run --tdx --vcpus 4 \
  --td-shim /path/to/td-shim.bin --kernel /path/to/tdx-bzImage \
  --image ubuntu:24.04 --entrypoint /bin/sh -- -c 'echo hello-from-tdx'
```

vCPU 上限受宿主 KVM、TD-Shim 和 guest 内核配置约束。TDX 保护 guest 的私有内存，宿主仍可访问 rootfs 文件、串口输出和 TAP 流量。真机测试使用非调试 TDREPORT 验证启动，远程认证需接入 Quote 与验证方。jailer 和快照适用于普通 KVM。 [TDX 固件、内核配置与限制](docs/guide.md#stage-13在-intel-tdx-中运行-oci-镜像)

## 开发与测试

运行时代码位于 [`src-rust/`](src-rust/)。开发时先运行与改动相关的 E2E，完整普通 KVM 套件使用 `make test`。 [测试约定](AGENTS.md)

```sh
sudo env VIRTIO_BZIMAGE=/path/to/bzImage make test-workload
sudo env SVMM_TEST_TDX_KERNEL=/path/to/tdx-bzImage \
  SVMM_TEST_TD_SHIM=/path/to/td-shim.bin make test-tdx
```

[使用与实现指南](docs/guide.md) 收录内核启动、virtio、网络、快照和消融实验。教学用 C 实现见 [`stage11`](https://github.com/billionairiam/SVMM/tree/stage11)。

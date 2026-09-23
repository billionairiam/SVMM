# Stage 03：从 initramfs 启动到交互 shell

根据 [Runra Stage 03 教程](https://runra.dev/zh/blog/sandbox-from-scratch-03-initramfs-shell) 实现：VMM 把静态 BusyBox 打包成的 initramfs 放进客户机内存，通过 `boot_params` 告诉内核位置，内核解包后运行 `/init`，最终在串口上得到一个可交互的 shell。它建立在 [Stage 02](https://runra.dev/zh/blog/sandbox-from-scratch-02-linux-boot) 的 bzImage 加载器之上。

内核入口仍然使用 [Linux 官方的 32 位启动协议](https://docs.kernel.org/arch/x86/boot.html#the-32-bit-boot-protocol)：设置 CPUID、保护模式和 GDT，以 `%esi` 传入 `boot_params`，从压缩内核入口开始执行。教程描述的 16 位 setup 入口在这个无 BIOS 的最小 VMM 中无法可靠完成启动。

## 构建与运行

需要 Linux x86_64、C 编译器、`/dev/kvm` 权限、`cpio`、`gzip`，以及一份可读取的 x86 bzImage。内核镜像和 initramfs 都不存入 Git。

```sh
make all initramfs           # initramfs 生成在 build/initramfs.cpio.gz
mkdir -p images
cp /boot/vmlinuz-6.16.0 images/bzImage  # 换成本机可读取的镜像
make run
```

`rootfs/build-rootfs.sh [输出路径]` 优先使用 `BUSYBOX` 环境变量或系统里的静态 BusyBox（没有 `PT_INTERP` 的 ELF），找不到时下载 busybox.net 的静态构建。它创建 applet 链接和 `/init`，再用 `cpio -H newc` + `gzip` 打包。`/init` 挂载 `proc`、`sysfs`、`devtmpfs`、`tmpfs`，打印 `hello from mini sandbox`，然后用 `setsid cttyhack /bin/sh` 让 shell 拿到 `/dev/ttyS0` 作为控制终端，所以 Ctrl-C 和作业控制都能用。

运行时：

- 参数为 `linux_boot [bzImage [initramfs]]`，也可以用 `BZIMAGE_PATH`、`INITRAMFS_PATH`、`CMDLINE` 环境变量。`INITRAMFS_PATH=` 为空表示不加载 initramfs。
- 默认命令行：`console=ttyS0 earlycon=uart8250,io,0x3f8 init=/init loglevel=8 acpi_rsdp=0xe0000 reboot=acpi panic=-1`。
- stdin 是终端时切换到 raw 模式，按键原样送进串口，由客户机处理行编辑和 Ctrl-C；按 **Ctrl-A x** 退出 VMM，按 Ctrl-A Ctrl-A 发送一个 Ctrl-A。退出时恢复终端模式。stdin 也可以是管道，例如 `printf 'uname -a\npoweroff -f\n' | ./bin/linux_boot`。
- 退出路径：`poweroff -f` 写 ACPI SLP_TYP=S5（`reason=poweroff`）；在 shell 中执行 `exit`、`reboot -f` 或内核 panic（`panic=-1`）时写 0xcf9 复位（`reason=reset`）；Ctrl-A x、SIGINT、SIGTERM 或 SIGHUP 让 VMM 停止 vCPU（`reason=host_stop`）。这些都视为正常结束，返回 0，并打印 `Stage 03 completed`。三重故障（`reason=shutdown`）和 KVM 错误返回非零。

## 测试

```sh
make test                                          # 单元测试；没有内核镜像时跳过 KVM 测试
BZIMAGE_PATH=/boot/vmlinuz-6.16.0 make test        # 加上 KVM 端到端测试
```

`tests/test_initramfs_shell.py` 是 Stage 03 的端到端测试，使用真实 KVM、内核和 initramfs：启动到提示符，执行命令并检查挂载、设备节点和 PID 1；用 13 KiB 的 heredoc 验证串口输入不丢字节；分别验证 `poweroff`、`exit` 复位、SIGTERM；在 pty 上验证控制终端、Ctrl-C、Ctrl-A x 和终端模式恢复；initramfs 不存在时 VMM 报错。`tests/test_linux_boot.sh` 用 `INITRAMFS_PATH=` 回归 Stage 02 的边界：内核 panic 在挂载根文件系统这一步，并通过复位干净退出。

## 启动布局

| 地址 | 内容 |
| --- | --- |
| `0x500` | 32 位启动用 GDT |
| `0x9000` | `boot_params` / zero page |
| `0x10000` | bzImage setup 段副本 |
| `0x20000` | 内核命令行 |
| `0xE0000` | RSDP（ACPI 2.0，只有 RSDT） |
| `0xE1000` / `0xE2000` / `0xE3000` / `0xE4000` | RSDT / FADT / DSDT / MADT |
| `0x100000` | bzImage 压缩内核载荷及入口 |
| `0x6000000`（96 MiB） | initramfs（`ramdisk_image` / `ramdisk_size`） |

客户机内存至少 256 MiB。initramfs 默认放在 96 MiB 处；如果内核首选运行地址加 `init_size` 越过这个位置，就改放到运行窗口之后，按 2 MiB 对齐。加载前检查 initramfs 不与内核运行窗口重叠、完整落在客户机内存里、不超过内核报告的 `initrd_addr_max`，内存大小也随之增加。e820 表仍是三项；0xE0000 处的 ACPI 表在内核眼里属于 640K–1M 的 BIOS 区，会被保留。

## 与教程的差异

教程的做法在发行版内核上看不到用户态的串口输出（教程自己也提到了这一点）。为了真正得到可交互的 shell，本实现做了以下调整：

- **在内核中创建中断控制器和时钟**：用 `KVM_CREATE_IRQCHIP`（LAPIC + IOAPIC + PIC）和 `KVM_CREATE_PIT2`。没有它们，HLT 会退出到 VMM，也没有定时器中断，调度器和用户态都跑不起来。HLT 改由 KVM 在内核中处理。CPUID 打开 TSC-deadline 位，并把 APIC ID 修正为 vCPU 编号。
- **UART 中断**：Linux 8250 驱动的发送和接收都靠中断驱动，所以串口实现了 IER/IIR 优先级、THRE/RDI 中断（受 MCR.OUT2 控制）、4 KiB 接收队列（满时阻塞输入线程形成背压）、FCR 清空、环回模式（`size_fifo()` 探测用，不输出到终端）。IRQ 4 通过 `KVM_IRQ_LINE` 注入。
- **HW-reduced ACPI**：FADT 设置 `HW_REDUCED_ACPI`，提供 SLEEP_CONTROL/STATUS（0x600/0x601）和 RESET_REG（0xcf9）；DSDT 由 iasl 编译，包含 `_S5` 和 COM1（PNP0501，IO 0x3f8，IRQ 4）。MADT 描述 CPU0 的 LAPIC 和 IOAPIC。内核通过 PnP ACPI 发现 ttyS0，`poweroff` 和复位都能退出 VMM。命令行加 `reboot=acpi`，否则在 HW-reduced 且没有 EFI 时，内核会先尝试 EFI 再退回 BIOS 实模式重启，panic 后会反复 double fault。
- **不覆盖 `initrd_addr_max`**：它是内核告诉加载器的上限，所以这里只读取并遵守，不像教程那样写入 0x0F000000。
- **未模拟的 IN 端口仍返回 0xff**，表示“没有设备”。教程改成返回 0，是为了绕开 16 位 setup 路径里的 PCI 探测循环；32 位入口没有这个问题，而 0 会让驱动误以为设备存在。
- **`/init` 保持 PID 1**，shell 在新会话中运行，这样才有作业控制；shell 退出后 `/init` 执行 `reboot -f`，整个 VMM 随之结束。

## Stage 02 消融实验

实验构建为 CPUID、动态内存、boot params、e820、内核命令行、保护模式/GDT 和 UART 分别生成一个只移除单项能力的二进制。普通 `make all` 不启用任何消融宏。

```sh
make ablation
experiments/stage02/run.sh
python3 experiments/stage02/collect.py --validate-results
```

默认主内核为 `/boot/vmlinuz-6.16.0`，兼容性内核为 `/boot/vmlinuz-6.18.0.bak`，单次运行上限为 15 秒。可以使用 `--primary-kernel`、`--compat-kernel`、`--performance-runs` 和 `--timeout` 覆盖这些值。完整实验通常需要约 2–5 分钟，具体取决于进入超时路径的变体数量。

结果保存在 `experiments/stage02/results/`：`raw.csv` 记录每次运行，`summary.csv` 保存聚合指标，`report.md` 给出结论，`logs/` 保留对应的 stdout 和 stderr。功能表覆盖全部变体；性能表只统计确认进入 Linux 且到达可识别终点的变体，loader 拒绝、三重故障和纯超时不会与正常启动耗时混合计算。

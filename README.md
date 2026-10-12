# Stage 04：最小 Sandbox API

根据 [Runra Stage 04 教程](https://runra.dev/zh/blog/sandbox-from-scratch-04-minimal-sandbox-api) 实现：把写死在 `main()` 里的启动流程整理成一个可配置的命令行工具 `svmm run`。内核、initramfs、内存大小和内核命令行都由参数决定，客户机内存布局随内存大小计算。它建立在 [Stage 03](https://runra.dev/zh/blog/sandbox-from-scratch-03-initramfs-shell)（initramfs + 交互 shell）和 [Stage 02](https://runra.dev/zh/blog/sandbox-from-scratch-02-linux-boot)（bzImage 加载器）之上。

代码分三层：

- `src/cli.c`：解析和校验参数，填好 `struct sandbox_config`。参数错误时在启动 VM 之前退出。
- `src/sandbox.c`：`sandbox_run(&config)` 按顺序执行加载内核 → 创建 VM → 分配内存 → ACPI → initramfs → boot_params → vCPU → 串口和控制台 → 运行，最后用一个 `cleanup` 出口统一释放资源。
- `src/main.c`：只有十来行，把前两者接起来。

内核入口仍然使用 [Linux 官方的 32 位启动协议](https://docs.kernel.org/arch/x86/boot.html#the-32-bit-boot-protocol)：设置 CPUID、保护模式和 GDT，以 `%esi` 传入 `boot_params`，从压缩内核入口开始执行。教程描述的 16 位 setup 入口在这个无 BIOS 的最小 VMM 中无法可靠完成启动。

## 构建与运行

需要 Linux x86_64、C 编译器、`/dev/kvm` 权限、`cpio`、`gzip`，以及一份可读取的 x86 bzImage。内核镜像和 initramfs 都不存入 Git。

```sh
make all initramfs           # 生成 bin/svmm 和 build/initramfs.cpio.gz
./bin/svmm run --kernel /boot/vmlinuz-6.16.0 \
    --initrd build/initramfs.cpio.gz --mem 512
```

也可以用 `make run`，默认值为 `KERNEL=images/bzImage INITRD=build/initramfs.cpio.gz MEM=256`，例如 `make run KERNEL=/boot/vmlinuz-6.16.0 MEM=512`。

`rootfs/build-rootfs.sh [输出路径]` 优先使用 `BUSYBOX` 环境变量或系统里的静态 BusyBox（没有 `PT_INTERP` 的 ELF），找不到时下载 busybox.net 的静态构建。它创建 applet 链接和 `/init`，再用 `cpio -H newc` + `gzip` 打包。`/init` 挂载 `proc`、`sysfs`、`devtmpfs`、`tmpfs`，打印 `hello from mini sandbox`，然后用 `setsid cttyhack /bin/sh` 让 shell 拿到 `/dev/ttyS0` 作为控制终端，所以 Ctrl-C 和作业控制都能用。

运行时：

- 命令行接口：

  ```text
  svmm run --kernel PATH [--initrd PATH] [--mem SIZE_MB] [--cmdline STR]
    -k, --kernel PATH    x86 bzImage（必需）
    -i, --initrd PATH    initramfs；不传则不加载，内核会停在挂载根文件系统
    -m, --mem SIZE_MB    客户机内存，单位 MiB，64–3072，默认 256
    -c, --cmdline STR    内核命令行，整体替换默认值
    -h, --help           打印帮助到 stdout 并返回 0
  ```

  参数错误（缺 `--kernel`、未知子命令或选项、`--mem` 不是纯十进制数或越界等）打印 `ERROR: ...` 到 stderr 并返回 2，不会创建 VM。
- 默认命令行：`console=ttyS0 earlycon=uart8250,io,0x3f8 init=/init loglevel=8 acpi_rsdp=0xe0000 reboot=acpi panic=-1`。
- stdin 是终端时切换到 raw 模式，按键原样送进串口，由客户机处理行编辑和 Ctrl-C；按 **Ctrl-A x** 退出 VMM，按 Ctrl-A Ctrl-A 发送一个 Ctrl-A。退出时恢复终端模式。stdin 也可以是管道，例如 `printf 'uname -a\npoweroff -f\n' | ./bin/svmm run -k ... -i ...`。管道输入要等 shell 提示符出现后再写，8250 驱动打开端口时会清空接收 FIFO。
- 退出路径：`poweroff -f` 写 ACPI SLP_TYP=S5（`reason=poweroff`）；在 shell 中执行 `exit`、`reboot -f` 或内核 panic（`panic=-1`）时写 0xcf9 复位（`reason=reset`）；Ctrl-A x、SIGINT、SIGTERM 或 SIGHUP 让 VMM 停止 vCPU（`reason=host_stop`）。这些都视为正常结束，返回 0，并打印 `sandbox completed`。三重故障（`reason=shutdown`）、KVM 错误和启动前的资源错误返回 1，同时打印 `SVMM_METRIC stage=failed_<步骤>`。

## 测试

```sh
make test                                          # 单元测试；没有内核镜像时跳过 KVM 测试
BZIMAGE_PATH=/boot/vmlinuz-6.16.0 make test        # 加上 KVM 端到端测试
```

`tests/test_sandbox_cli.py` 是 Stage 04 的端到端测试，文件开头列出了要防住的失败方式。它直接运行 `bin/svmm`：不需要 KVM 的部分检查帮助、各种错误参数的退出码和输出；KVM 部分用 `--mem 512` 和自定义 `--cmdline` 启动，从内核日志核对 e820 末尾、`RAMDISK` 地址、`MemTotal` 和 `/proc/cmdline`；用 `--mem 128` 确认 initramfs 避开内核解压窗口且仍能进入 shell；用 `--mem 64` 确认放不下时在启动前报错。

`tests/test_initramfs_shell.py` 是 Stage 03 的端到端测试，使用真实 KVM、内核和 initramfs：启动到提示符，执行命令并检查挂载、设备节点和 PID 1；用 13 KiB 的 heredoc 验证串口输入不丢字节；分别验证 `poweroff`、`exit` 复位、SIGTERM；在 pty 上验证控制终端、Ctrl-C、Ctrl-A x 和终端模式恢复；initramfs 不存在时 VMM 报错。`tests/test_linux_boot.sh` 不传 `--initrd`，回归 Stage 02 的边界：内核 panic 在挂载根文件系统这一步，并通过复位干净退出。

## 启动布局

| 地址 | 内容 |
| --- | --- |
| `0x500` | 32 位启动用 GDT（GDTR 指向这里，CS=0x10、DS=0x18 取自其中两项） |
| `0x9000` | `boot_params` / zero page（e820 表在其内部偏移 0x2d0） |
| `0x10000` | bzImage setup 段副本（只供检查，不执行） |
| `0x20000` | 内核命令行 |
| `0x90000` | 入口临时栈顶（RSP 初值） |
| `0xE0000` | RSDP（ACPI 2.0，只有 RSDT） |
| `0xE1000` / `0xE2000` / `0xE3000` / `0xE4000` | RSDT / FADT / DSDT / MADT |
| `0x100000` | bzImage 压缩内核载荷及入口 |
| `max(3/8 内存, 16 MiB, 内核窗口末尾)`，2 MiB 对齐 | initramfs（`ramdisk_image` / `ramdisk_size`） |
| 内存末尾 16 MiB | 预留，不放 initramfs |

客户机内存由 `--mem` 决定，范围 64–3072 MiB；上限让 RAM 留在 IOAPIC/LAPIC 的 MMIO 区和 32 位 `ramdisk_image` 之下。initramfs 的起点按教程取内存的 3/8（256 MiB 时为 96 MiB，512 MiB 时为 192 MiB），但不低于内核解压窗口（首选运行地址 + `init_size`）的末尾；它必须在 `min(内存 − 16 MiB, initrd_addr_max + 1)` 之前结束，否则在启动前报错并提示加大 `--mem`。内核窗口本身超出内存时同样在启动前报错。e820 表是三项，最后一项为 `[1 MiB, 内存末尾)`；0xE0000 处的 ACPI 表在内核眼里属于 640K–1M 的 BIOS 区，会被保留。

这些地址集中定义在 `src/memory.h` 和 `src/boot/acpi.h`；`src/boot/linux.c` 中 `boot_linux_prepare()` 前的注释画出了完整布局。

## 看懂 VM exit

VMM 的核心是 `vcpu_run()` 里的循环：`KVM_RUN` 让客户机在真实 CPU 上运行，直到客户机做了一件 KVM 需要用户态帮忙的事（例如访问串口端口），这时 `KVM_RUN` 返回，这就是一次 VM exit。VMM 模拟完这次访问后再次调用 `KVM_RUN`。

每次运行结束时，VMM 在 stderr 打印退出统计：按退出原因分类，I/O 退出再按端口分类，并标出端口属于哪个设备、是否有模拟。下面是不带 initramfs 启动 6.16 内核的输出节选：

```text
VM exits handled by the VMM: 38526 (exits KVM handles in the kernel are not counted)
  exit reason                     exits    share
  KVM_EXIT_IO                     38526  100.00%
  I/O exits by port (one REP INS/OUTS counts once):
  port     dir       exits  device
  0x03f8   out       19405  COM1 THR/DLL (transmit byte)
  0x03fd   in        18278  COM1 LSR (line status)
  0x0cf8   out          69  PCI config address (not emulated)
  ...
```

可以从中读出：

- 内核约 19 KB 的串口输出，带来约 3.8 万次退出：每输出一个字符，先读一次 LSR 确认可以发送，再写一次 THR。逐字节陷出的端口 I/O 很昂贵，这也是 virtio 这类半虚拟化设备要批量传输的原因。
- 没有模拟的设备（PCI、COM2–4 等）只会被探测几次：读到 0xff 后，内核就认为设备不存在。
- 表里只有退出到用户态的那部分。中断控制器、PIT、HLT、EPT 缺页等由 KVM 在内核里处理，不会出现在这里；要看它们可以用 `perf kvm stat`。

## 与教程的差异

Stage 04 部分：

- **initramfs 地址不低于内核解压窗口末尾**。教程只取 3/8 内存。发行版内核的 `init_size` 很大（6.16 为 40 MiB，6.18 为 62 MiB，7.0 为 68 MiB，窗口从 16 MiB 开始），`--mem 128` 时 3/8 = 48 MiB 会落进窗口，内核解压时覆盖 initramfs，客户机直接挂起。
- **仍然使用 32 位入口**，不采用教程的实模式 setup 入口（CS=0x1000、RIP=0x200），原因见开头。教程为此增加的 DMA 端口范围也就不需要了。
- **`--initrd` 是可选的，不传就不加载**，不会去找默认路径。这样 Stage 02 的“停在挂载根文件系统”边界仍能用同一个二进制测试。
- **内存上限 3072 MiB**，并要求 `--mem` 为纯十进制数；教程没有上限。
- **加载内核放在创建 VM 之前**，内核文件不存在时不需要 `/dev/kvm` 就能报错，失败指标的阶段名和顺序与 Stage 02 保持一致。
- 二进制叫 `bin/svmm`（教程叫 `mini-sandbox`，这里与项目名一致）；CLI 测试写成 Python 端到端测试 `tests/test_sandbox_cli.py`，而不是教程的 `test_sandbox_cli.sh`。
- `KVM_SET_TSS_ADDR` 失败只打印警告，与教程一致。

Stage 03 部分：教程的做法在发行版内核上看不到用户态的串口输出（教程自己也提到了这一点）。为了真正得到可交互的 shell，本实现做了以下调整：

- **在内核中创建中断控制器和时钟**：用 `KVM_CREATE_IRQCHIP`（LAPIC + IOAPIC + PIC）和 `KVM_CREATE_PIT2`。没有它们，HLT 会退出到 VMM，也没有定时器中断，调度器和用户态都跑不起来。HLT 改由 KVM 在内核中处理。CPUID 打开 TSC-deadline 位，并把 APIC ID 修正为 vCPU 编号。
- **UART 中断**：Linux 8250 驱动的发送和接收都靠中断驱动，所以串口实现了 IER/IIR 优先级、THRE/RDI 中断（受 MCR.OUT2 控制）、4 KiB 接收队列（满时阻塞输入线程形成背压）、FCR 清空、环回模式（`size_fifo()` 探测用，不输出到终端）。IRQ 4 通过 `KVM_IRQ_LINE` 注入。
- **HW-reduced ACPI**：FADT 设置 `HW_REDUCED_ACPI`，提供 SLEEP_CONTROL/STATUS（0x600/0x601）和 RESET_REG（0xcf9）；DSDT 由 iasl 编译，包含 `_S5` 和 COM1（PNP0501，IO 0x3f8，IRQ 4）。MADT 描述 CPU0 的 LAPIC 和 IOAPIC。内核通过 PnP ACPI 发现 ttyS0，`poweroff` 和复位都能退出 VMM。命令行加 `reboot=acpi`，否则在 HW-reduced 且没有 EFI 时，内核会先尝试 EFI 再退回 BIOS 实模式重启，panic 后会反复 double fault。
- **不覆盖 `initrd_addr_max`**：它是内核告诉加载器的上限，所以这里只读取并遵守，不像教程那样写入 0x0F000000。
- **未模拟的 IN 端口仍返回 0xff**，表示“没有设备”。教程改成返回 0，是为了绕开 16 位 setup 路径里的 PCI 探测循环；32 位入口没有这个问题，而 0 会让驱动误以为设备存在。
- **`/init` 保持 PID 1**，shell 在新会话中运行，这样才有作业控制；shell 退出后 `/init` 执行 `reboot -f`，整个 VMM 随之结束。

## Stage 02 消融实验

实验构建（`bin/ablation/<变体>/svmm`）为 CPUID、动态内存、boot params、e820、内核命令行、保护模式/GDT 和 UART 分别生成一个只移除单项能力的二进制。普通 `make all` 不启用任何消融宏。`fixed_32m` 变体忽略 `--mem`，固定使用教程 Stage 02 的 32 MiB。实验脚本只传 `run --kernel`，不加载 initramfs。

```sh
make ablation
experiments/stage02/run.sh
python3 experiments/stage02/collect.py --validate-results
```

默认主内核为 `/boot/vmlinuz-6.16.0`，兼容性内核为 `/boot/vmlinuz-6.18.0.bak`，单次运行上限为 15 秒。可以使用 `--primary-kernel`、`--compat-kernel`、`--performance-runs` 和 `--timeout` 覆盖这些值。完整实验通常需要约 2–5 分钟，具体取决于进入超时路径的变体数量。

注意：`experiments/stage02/results/` 里的数据是 Stage 02 时采集的，当时还没有内核 irqchip，HLT 等也会退出到 VMM，所以 exit 数（约 21 万次）比现在多得多。

结果保存在 `experiments/stage02/results/`：`raw.csv` 记录每次运行，`summary.csv` 保存聚合指标，`report.md` 给出结论，`logs/` 保留对应的 stdout 和 stderr。功能表覆盖全部变体；性能表只统计确认进入 Linux 且到达可识别终点的变体，loader 拒绝、三重故障和纯超时不会与正常启动耗时混合计算。

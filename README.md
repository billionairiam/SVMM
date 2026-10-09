# Stage 02：在 KVM 中启动 Linux bzImage

根据 [Runra Stage 02 教程](https://runra.dev/zh/blog/sandbox-from-scratch-02-linux-boot) 实现的 Linux 加载器。它读取 x86 bzImage，将 setup 段和压缩内核分别加载到客户机物理地址 `0x10000`、`0x100000`，构造 `boot_params`、命令行和 e820 内存表，再进入内核。

本实现使用 [Linux 官方的 32 位启动协议](https://docs.kernel.org/arch/x86/boot.html#the-32-bit-boot-protocol)：给 vCPU 设置 CPUID、保护模式和 GDT，以 `%esi` 传入 `boot_params`，从压缩内核入口开始执行。教程描述的 16 位 setup 入口（`CS=DS=ES=0x1000`、`IP=0x200`，即 setup 偏移 `0x200`）依赖 BIOS 中断服务（如 `int 0x15` 查询 e820、`int 0x10` 设置显示）；这个 VMM 没有 BIOS，按教程的寄存器设置实测 15 秒内既没有串口输出也没有 VM exit。setup 段仍会加载到内存，供检查镜像布局。

## 构建与运行

需要 Linux x86_64、C 编译器、`/dev/kvm` 权限，以及一份可读取的 x86 bzImage。内核镜像不存入 Git。

```sh
make all
mkdir -p images
cp /boot/vmlinuz-6.16.0 images/bzImage  # 换成本机可读取的镜像
make run
make test
```

也可以直接指定镜像与命令行：

```sh
BZIMAGE_PATH=/boot/vmlinuz-6.16.0 \
CMDLINE='console=ttyS0 earlycon=uart8250,io,0x3f8' \
./bin/linux_boot
```

默认命令行包含 `console=ttyS0` 和 `earlycon`，以便在支持 8250 控制台的内核上看到启动日志。`make test` 总会运行 bzImage 解析、boot_params、串口寄存器、CMOS RTC 和端口 I/O 分发测试；设置 `BZIMAGE_PATH` 后还会运行 KVM 集成测试。内核日志到达串口时，集成测试还会检查客户机通过 RTC 读到的日期等于宿主机 UTC 日期，并且没有出现 `Stale IRR`、`APIC ID mismatch` 等 APIC 相关错误。对已知内置 8250 控制台的镜像，可运行更严格的验证：

```sh
BZIMAGE_PATH=/boot/vmlinuz-6.16.0 REQUIRE_KERNEL_LOG=1 make test
```

## 启动布局

| 地址 | 内容 |
| --- | --- |
| `0x500` | 32 位启动用 GDT |
| `0x9000` | `boot_params` / zero page |
| `0x10000` | bzImage setup 段副本 |
| `0x20000` | 内核命令行 |
| `0x100000` | bzImage 压缩内核载荷及入口 |

客户机内存至少 128 MiB；若内核首选运行地址加 `init_size` 超出该范围，则继续增加。这里比教程的 32 MiB 大，因为较新的内核仅初始化空间就可能超过 32 MiB。e820 表根据实际分配的内存填写。

本阶段没有 initramfs、根文件系统和完整的中断控制器。VMM 在客户机第一次 `hlt` 时退出；`Stage 02 completed` 表示本阶段的运行循环结束。部分内核会在根文件系统挂载失败后持续停留在 panic 中，此时可以用 `timeout 15 make run` 限制运行时间；集成测试会将出现明确 VFS 根文件系统 panic 的情况视为达到本阶段预期边界。客户机 shutdown 会被报告为错误，因为它也可能由三重故障引起。

## 设备模型

本阶段没有调用 `KVM_CREATE_IRQCHIP`，所有设备都在用户态的 `vcpu_run()` 中模拟：

| 端口 / 地址 | 处理方式 |
| --- | --- |
| `0x3f8`–`0x3ff` | 16550 UART（`src/serial.c`），THR 写入输出到 stdout |
| `0x70`、`0x71` | 最小 MC146818 CMOS RTC（`src/rtc.c`），时间取宿主机 UTC |
| `0x80` | POST 诊断端口，Linux 用它做 I/O 延时，静默接受 |
| 其他端口 | 写入丢弃、读回 `0xff`，每个端口只在第一次访问时记录一行日志 |
| MMIO | 写入丢弃、读回全 1，最多记录 8 行日志 |

端口设备都是 8 位寄存器：宽度为 2/4 字节的 `in`/`out` 把第 j 个字节分发到 `port + j`，`rep ins/outs` 的每个元素都从同一个起始端口开始。

因为没有 LAPIC，VMM 会：

- 把 `APIC_BASE` MSR 清零（`sregs.apic_base = 0`）。仅去掉 CPUID 的 APIC 位不够，KVM 会根据 `APIC_BASE` 的使能位重新设置它。
- 在 CPUID 中去掉 APIC、x2APIC 和 TSC-deadline 特性，并把 leaf 1/`0xb`/`0x1f` 中的初始 APIC ID 改为 vCPU 编号。

否则 Linux 会访问 `0xfee00000` 处的 LAPIC 寄存器，每次访问都是一次 MMIO exit，读回的全 1 还会让内核报 `Stale IRR`。同理，没有 RTC 时 `0x71` 读回 `0xff`，UIP 位看上去一直置位，内核会反复轮询直到超时，最后报 `Unable to read current time from RTC`。

消融实验中 `baseline` 变体使用 `/boot/vmlinuz-6.16.0` 运行到首次 `hlt` 的 10 次均值（见 `experiments/stage02/results/report.md`）：

| 版本 | KVM exits | 墙钟时间均值 |
| --- | ---: | ---: |
| 只有 UART，LAPIC 仍启用 | 209,384 | 2160 ms |
| 加入 RTC，禁用 LAPIC | 29,786 | 377 ms |

剩余的 exit 几乎都来自串口逐字节输出。

## Stage 02 消融实验

实验构建为 CPUID、动态内存、boot params、e820、内核命令行、保护模式/GDT 和 UART 分别生成一个只移除单项能力的二进制。普通 `make all` 不启用任何消融宏。

```sh
make ablation
experiments/stage02/run.sh
python3 experiments/stage02/collect.py --validate-results
```

默认主内核为 `/boot/vmlinuz-6.16.0`，兼容性内核为 `/boot/vmlinuz-6.18.0.bak`，单次运行上限为 15 秒。可以使用 `--primary-kernel`、`--compat-kernel`、`--performance-runs` 和 `--timeout` 覆盖这些值。完整实验通常需要约 2–5 分钟，具体取决于进入超时路径的变体数量。

结果保存在 `experiments/stage02/results/`：`raw.csv` 记录每次运行，`summary.csv` 保存聚合指标，`report.md` 给出结论，`logs/` 保留对应的 stdout 和 stderr。功能表覆盖全部变体；性能表只统计确认进入 Linux 且到达可识别终点的变体，loader 拒绝、三重故障和纯超时不会与正常启动耗时混合计算。

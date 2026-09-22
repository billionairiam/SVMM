# Stage 02：在 KVM 中启动 Linux bzImage

根据 [Runra Stage 02 教程](https://runra.dev/zh/blog/sandbox-from-scratch-02-linux-boot) 实现的 Linux 加载器。它读取 x86 bzImage，将 setup 段和压缩内核分别加载到客户机物理地址 `0x10000`、`0x100000`，构造 `boot_params`、命令行和 e820 内存表，再进入内核。

本实现使用 [Linux 官方的 32 位启动协议](https://docs.kernel.org/arch/x86/boot.html#the-32-bit-boot-protocol)：给 vCPU 设置 CPUID、保护模式和 GDT，以 `%esi` 传入 `boot_params`，从压缩内核入口开始执行。教程描述的 16 位 setup 入口在这个无 BIOS 的最小 VMM 中无法可靠完成启动；实测会在没有任何串口日志的情况下停住。setup 段仍会加载到内存，供检查镜像布局。

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

默认命令行包含 `console=ttyS0` 和 `earlycon`，以便在支持 8250 控制台的内核上看到启动日志。`make test` 总会运行 bzImage 解析、boot_params 与串口寄存器测试；设置 `BZIMAGE_PATH` 后还会运行 KVM 集成测试。对已知内置 8250 控制台的镜像，可运行更严格的验证：

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

客户机内存至少 128 MiB；若镜像的 `init_size` 更大，则继续增加。这里比教程的 32 MiB 大，因为较新的内核仅初始化空间就可能超过 32 MiB。e820 表根据实际分配的内存填写。

本阶段没有 initramfs、根文件系统和完整的中断控制器。VMM 在客户机第一次 `hlt` 或 shutdown 时退出；`Stage 02 completed` 表示本阶段的运行循环结束，不代表内核已进入用户空间。

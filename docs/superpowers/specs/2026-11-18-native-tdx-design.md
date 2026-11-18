# 原生 KVM TDX workload 启动设计

用户选定原生 KVM TDX。目标是在支持 TDX 的宿主机上，通过 `svmm run --tdx --image IMAGE` 用 SVMM 自己的 C VMM 启动 Linux Trust Domain，运行镜像的默认应用，并把应用退出状态交回 CLI。

## 现状与方案

- 宿主机运行 Linux 7.0.0-31-generic，KVM 已启用 TDX，报告 VM 类型位图 `0x23`。本次用 `KVM_CREATE_VM(KVM_X86_TDX_VM)` 获得并关闭了真实 TD VM fd。该 VM 的 TDX_CAPABILITIES 查询成功，属性掩码为 0x10000000，宿主也报告 guest_memfd、私有内存属性和 split irqchip 支持。此探测验证了创建接口，尚未验证固件启动。现有 SVMM 走普通 KVM 的 32 位 bzImage 启动、宿主 RAM 映射和内核 irqchip。
- 本机有 TD-Shim 产物 `/home/ubuntu/yaoji/td-shim/target/release/final.bin` 和至少一份启用内建 TDX guest、virtio-mmio、ext4、SWIOTLB、TDX guest driver 的候选内核 `/boot/vmlinuz-6.18.0.bak`。本机固件声明 TDVF v1 metadata，包含独立 BFV、TD_HOB、payload 与命令行区域。产物的实际启动兼容性由首批 E2E 确认。如该产物不兼容，使用本地 libkrun 验证报告记录的上游 TD-Shim 版本 0362d0bc6d784170af28551e0b9bc6fd5ad414d2 和 boot_kernel 构建方式。
- 原生 SVMM + TD-Shim（已选）让现有设备模型和启动生命周期继续由 SVMM 管理。需要新增 TD 初始化、固件交接、共享内存和中断处理。
- 也可把工作负载交给现有 libkrun-tdx，节省设备适配，但运行 VM 的会是 libkrun 后端。已安装的 QEMU 8.2 对当前宿主 KVM 的 TDX ABI 报不支持。可把本地 libkrun 的 TD-Shim 路径作为实现参考，SVMM 产物不依赖该仓库或其动态库。

## 入口和范围

- 首版支持 Linux x86_64、一颗 vCPU、TD-Shim、现有 OCI ext4 rootfs 和串口。命令为 `svmm run --tdx --image IMAGE`，并支持低层的 `svmm run --tdx --kernel PATH --initrd PATH [--disk PATH] [--workload]`。低层无盘路径用来验证真实 TD 引导。
- 固件从 `--td-shim PATH`、`SVMM_TD_SHIM` 或仓库 `images/td-shim.bin` 取值。优先使用显式 `--kernel` 或 `SVMM_KERNEL`，自动选择时在原有 workload 内核条件上检查 TDX 所需的内建配置，包括 TDX guest、KVM guest、x2APIC、SWIOTLB 和内核介导的退出通道。调试环境使用已确认的内核配置与固件产物。预检缺失固件或 KVM 能力时立即失败，`--image` 路径在拉取前运行该预检。任何 TDX 初始化失败都禁止自动降级启动普通 VM。
- CLI 支持镜像默认命令及已有覆盖选项。首版 TDX 模式明确拒绝 `--tap`、`--netns`、`--jail`、`--snapshot`、`--warm-save`、`--restore`。先跑通离线 OCI 应用，网络、jail 和快照各自需要独立验证。普通 KVM 命令保持原有参数和行为。
- 目标是运行非调试 TD，并在硬件 E2E 中调用 guest 的 `TDX_CMD_GET_REPORT0`。首版不提供远程 Quote、QGS、密钥下发、完整镜像身份验证或磁盘加密。镜像 rootfs 由宿主提供，宿主可见磁盘、串口和 guest 共享的 I/O 缓冲区。TDREPORT 是本地报告，只有建立测量策略和远程验证后才可用于向不可信宿主交付秘密。

## 启动和设备

1. 在普通路径旁添加 TDX backend。加载与运行中的宿主匹配的 KVM ABI，检查 VM type、guest_memfd、memory attributes 和所需的 split irqchip / hypercall 能力。复用当前运行循环的设备分派，TD 初始化跳过普通路径的 `KVM_SET_REGS`、`KVM_SET_SREGS` 和完整内核 irqchip。按 ABI 调用 TD capabilities、INIT_VM、INIT_VCPU、INIT_MEM_REGION、FINALIZE_VM。INIT_VM 在创建 vCPU 前设置 TD 的 CPUID 和 XFAM，在 INIT_VCPU 前设置与 TD 一致且启用 x2APIC 的 vCPU CPUID。TD attributes 清除 DEBUG，启用宿主支持的 SEPT #VE disable。INIT_MEM_REGION 在 vCPU fd 上执行，处理部分进度和 EINTR，不能重复导入已经完成的页。报错区分宿主能力、初始化子命令和运行失败。
2. 解析 TD-Shim 固件元数据，校验文件和 GPA 的范围、对齐及重叠，用独立高地址映射放固件和 reset vector。为 TD-Shim 生成 TD HOB，提供内存图和 ACPI 表，按 64 位 Linux boot protocol 装载 kernel 与 initrd。以 RawVmLinux payload 提供可控的启动入口，经小 trampoline 补齐 shim 未设置的 initrd 和所用内核要求的 boot_params 字段，地址保持在 4 GiB 以下。FADT、DSDT、MADT 通过 ACPI GUID HOB 交给固件，固件重新定位 DSDT 前清除 FADT 中普通路径使用的固定 X_DSDT 指针，验证重新定位后的指针和 MADT 中的 IOAPIC 声明。固件和启动 payload 的初始化及测量范围应明确，测量初始化页面与 eventual 数据盘是两件独立的事。
3. 低地址 RAM 使用 guest_memfd 和 KVM 私有属性。处理 TD guest 的 MapGPA 请求，校验页对齐、长度、溢出、地址属性和 RAM 范围，按运行中的 KVM ABI 切换并返回结果，释放不再有效的旧 backing，保持设备访问与转换同步；固件映射不能转换。记录哪些页面被 guest 转成共享。TD 模式的设备地址转换仅接受实际共享的完整 DMA 范围，明确处理 shared GPA 位和内核返回规范化地址的场合。virtio-mmio 在 TD 模式协商 ACCESS_PLATFORM，利用 guest DMA/SWIOTLB；队列注册和处理都按共享范围校验，避免持有跨转换有效的原始指针。现有块设备命令和普通 VM 地址转换可复用。
4. 用 split irqchip 保留 KVM local APIC，SVMM 处理 ACPI 声明的 IOAPIC 寄存器和中断路由。TD vCPU 不提供普通 split irqchip 所依赖的 EOI 通知。TD 路径将串口和 virtio-blk 声明为边沿触发，经 KVM MSI 投递，并验证连续请求、屏蔽及重新使能后的完成。能收到启动日志不代表磁盘中断可用，必须分别验收。处理 TDX fatal 和未知退出，明确失败。
5. 继续使用独立静态 guest init 管应用。直接从 guest 用户态执行旧 `outl` 可能触发 Linux 不处理的用户态 I/O #VE，TD 路径采用经内核介导的端口写入。首选验证 guest `/dev/port`，PID 1 完成同步后送有固定起始字节、顺序和状态的完成帧。Linux 将写入分为顺序的 byte I/O，TD 的 VMM 分支独立组帧。应用不继承 PID 1 的 fd。接口需要内建 DEVPORT 和允许该接口的 guest 配置；完成帧不用于认证特权 guest。VMM 校验帧、返回真实退出状态，业务串口日志不能触发结束。如果内核路径不成立，先用真实硬件定位并调整到独立半虚拟通道，再宣布 workload 入口可用。

## 先写 E2E，再实现

- **TD 创建后无法引导。** 在真实硬件上启动选定固件和测试 initramfs，要求 Linux 识别自己运行于 TDX、guest 读取带测试 nonce 的 TDREPORT，检查返回的 REPORTDATA 与预期相同、TDINFO 的 DEBUG 位为零，再输出探针完成标记。宿主必须走 `KVM_X86_TDX_VM` 并完成初始化测量。普通 KVM 无法通过这条验证。
- **内核起来，磁盘 I/O 卡住或读错。** 用真实镜像包含最小静态程序，经 `run --tdx --image` 启动。程序校验输入文件内容、写入新文件并同步，返回 7。测试校验串口输出、宿主退出码以及低层保留磁盘的内容和 ext4 文件系统一致性。默认临时磁盘退出后应清理。
- **TDX 参数被绕过或输入错误后走普通启动。** 检查缺失和损坏固件、未支持的组合，要求在产生镜像副作用或进入普通 KVM 前报错；无 TDX 宿主上执行能力错误路径。保留现有普通 KVM 启动和本地镜像 E2E 作为相关回归。
- **两个环境采用不同 ABI。** 首先在本机 7.0.0-31 上验证启动。供应商较早的实验 ABI 不列入本期。另一台宿主如需支持，用该宿主的 KVM API 与同样的 E2E 检查。

开发时按引导、块设备、workload 逐步跑对应 E2E，不加只检查常量或源码文本的单测。具体文件分工、硬件 E2E 命令、内核和固件复现来源在实现计划与 README 中落定。

参考：[Linux KVM TDX ABI](https://docs.kernel.org/virt/kvm/x86/intel-tdx.html)、[TD-Shim 固件交接规范](https://github.com/confidential-containers/td-shim/blob/main/doc/tdshim_spec.md)、[Linux TDX guest API](https://docs.kernel.org/virt/coco/tdx-guest.html)。

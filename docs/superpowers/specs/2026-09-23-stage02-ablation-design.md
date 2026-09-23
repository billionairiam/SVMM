# Stage 02 消融实验设计

## 目标

为 SVMM Stage 02 建立可复现的消融实验，分别移除 Linux bzImage 启动路径中的关键组件，测量组件对启动功能和运行性能的影响。实验产物包括编译期变体、自动运行脚本、原始 CSV、汇总 CSV、逐次日志和 Markdown 报告，全部保存在 `stage02` 分支。

实验结论只描述当前 SVMM 实现、宿主机 KVM 和指定测试内核。超时只表示在观察窗口内没有终止，不能单独证明客户机崩溃。

## 基本原则

- 使用编译期宏生成消融变体，每个变体只移除一个组件。
- 默认 `make` 和 `bin/linux_boot` 不带消融宏，行为保持不变。
- 每个变体使用独立构建目录，禁止复用不同宏配置产生的目标文件。
- 失败、拒绝和超时都是有效实验结果，单个变体不得中断整批实验。
- 功能结果与性能结果分开解释；无法进入内核的变体不参与正常启动性能均值比较。

## 实验矩阵

| 变体 | 编译宏 | 移除内容 | 主要问题 |
| --- | --- | --- | --- |
| `baseline` | 无 | 无 | Stage 02 的基准行为和性能是什么？ |
| `no_cpuid` | `SVMM_ABLATE_CPUID` | `KVM_SET_CPUID2` | Linux 启动是否依赖 KVM 提供的 CPUID？ |
| `fixed_32m` | `SVMM_ABLATE_DYNAMIC_MEMORY` | 按启动头动态确定内存大小 | 博客中的固定 32 MiB 是否足以启动当前内核？ |
| `no_boot_params` | `SVMM_ABLATE_BOOT_PARAMS` | zero page / `boot_params` 内容 | 32 位入口缺少启动参数时会在哪里停止？ |
| `no_e820` | `SVMM_ABLATE_E820` | e820 条目 | 内核能否在没有可用内存表的情况下继续初始化？ |
| `no_cmdline` | `SVMM_ABLATE_CMDLINE` | 命令行内容和指针 | 串口控制台参数对可观测启动过程有什么影响？ |
| `no_protected_mode` | `SVMM_ABLATE_PROTECTED_MODE` | GDT 和 32 位保护模式设置 | 压缩内核入口是否能在错误 CPU 模式下执行？ |
| `no_uart` | `SVMM_ABLATE_UART` | 8250 UART 端口模型 | 客户机执行与宿主机串口可观测性如何变化？ |

`fixed_32m` 是博客原始 32 MiB 假设的对照组。基线继续根据 `runtime_start + init_size` 计算足够的内存，以便本机现代内核能够进入启动流程。

## 代码结构

### 消融配置

新增 `src/ablation.h`，将编译宏转换为清晰的布尔常量或条件块。生产构建不定义任何 `SVMM_ABLATE_*` 宏。

条件分别放在组件实际生效的位置：

- `src/main.c`：动态内存与固定 32 MiB 的选择。
- `src/boot/linux.c`：`boot_params`、e820 和命令行装载。
- `src/vcpu.c`：CPUID、保护模式/GDT 和 UART I/O 分发。

消融代码不得绕过边界检查后继续写越界内存。`fixed_32m` 若无法容纳镜像声明的初始化窗口，应得到 `loader_rejected`，不能通过关闭检查制造未定义行为。

### 构建隔离

扩展 Makefile，使实验构建接受变体名和对应宏，将产物放到：

```text
build/ablation/<variant>/
bin/ablation/<variant>/linux_boot
```

依赖文件也位于各自构建目录，确保切换变体不会错误复用目标文件。默认 `build/` 和 `bin/linux_boot` 保持现状。

### 运行与采集

新增：

```text
experiments/stage02/run.sh
experiments/stage02/results/raw.csv
experiments/stage02/results/summary.csv
experiments/stage02/results/report.md
experiments/stage02/results/logs/
```

`run.sh` 完成以下步骤：

1. 构建全部变体。
2. 对主内核执行每个变体的一次功能实验。
3. 对能够稳定进入内核的变体执行 10 次性能实验。
4. 对兼容性内核执行每个变体的一次功能实验。
5. 从宿主机诊断和客户机串口日志提取指标。
6. 写入原始数据、汇总数据和报告。

脚本采用临时文件收集单次运行结果，在一轮完成后原子写入结果文件，避免中断后留下半行 CSV。每次运行最长 15 秒；超时后终止对应进程并继续下一项。

## 测试内核

- 主实验镜像：`/boot/vmlinuz-6.16.0`
- 兼容性镜像：`/boot/vmlinuz-6.18.0.bak`

脚本允许通过参数覆盖这两个路径。结果文件记录内核路径、文件大小以及可获取时的 SHA-256，避免把不同镜像的结果混在一起。

6.16 内核用于重复性能测量，因为当前基线会稳定到达 `KVM_EXIT_HLT`。6.18 内核用于功能兼容性验证；在没有 initramfs 和根文件系统时，它可能到达 VFS panic 后保持运行，因此超时可以与明确的 panic 标记共同出现。

## 采集指标

每次运行在 `raw.csv` 中占一行，至少包含：

- 内核标识和变体名
- 运行轮次与实验类型
- 客户机内存字节数
- 是否进入 `KVM_RUN`
- 进程退出码和结果分类
- 是否出现 `Linux version`
- 是否出现 BIOS-e820 日志
- 是否出现串口输出
- 是否出现明确的 kernel panic
- KVM exit 总次数
- 串口 I/O exit 次数
- 墙钟时间，单位毫秒
- 最大宿主机 RSS，单位 KiB
- stdout 和 stderr 日志路径

`summary.csv` 对“同一内核、同一变体、同一实验类型”聚合，包含运行次数、各结果数量、成功率，以及墙钟时间、KVM exit、串口 exit 和 RSS 的平均值、最小值、最大值。

## 结果分类

分类按以下优先级执行，保证同一运行只有一个主要结果：

1. `loader_rejected`：进入 `KVM_RUN` 前因镜像或内存布局被拒绝。
2. `triple_fault`：收到 `KVM_EXIT_SHUTDOWN`。
3. `panic`：日志包含明确的 `Kernel panic - not syncing:`。这包括 VFS 根文件系统 panic，也包括消融组件后暴露出的更早期 panic。
4. `halted`：收到 `KVM_EXIT_HLT`。
5. `booted`：出现 `Linux version`，但没有出现以上终止状态。
6. `timeout`：15 秒到期且没有更具体的已识别状态。
7. `error`：其他宿主机错误或未知退出。

除主要结果外，`Linux version`、e820、串口和 panic 仍作为独立布尔指标保存。例如，6.18 可以主要分类为 `panic`，并同时记录该进程最终由超时终止。

## 性能比较

功能实验覆盖所有变体。只有满足以下条件的变体进入 10 次性能实验：

- 功能实验出现 `Linux version`；
- 结果可由 `halted`、`panic` 或明确的 Stage 02 终点识别；
- 不依赖未解释的宿主机错误结束。

报告分别展示功能表和性能表。性能表不把 `loader_rejected`、`triple_fault` 或纯超时与正常启动耗时放在同一均值中。对于 `no_uart` 和 `no_cmdline`，如果没有客户机日志但能由 KVM exit 确认终点，报告会明确标记“客户机进度不可由串口验证”。

## 可观测性改动

为避免从不稳定的自然语言日志猜测结果，VMM 在 stderr 输出稳定的机器可解析记录，前缀为 `SVMM_METRIC`。记录至少包括：

- 当前阶段
- 客户机内存大小
- 最终 KVM exit
- KVM exit 总数
- 串口 exit 数

人类可读日志继续保留。机器记录不改变客户机状态，也不参与正常控制流。

## 验证

实施完成后执行：

1. `make clean && make all`
2. `make test`
3. 使用 6.16 内核运行严格集成测试。
4. 使用 6.18 内核运行严格集成测试。
5. 运行完整消融脚本并确认所有变体都有原始数据和日志。
6. 校验 CSV 列数、数值字段和报告汇总与原始数据一致。
7. 在实验完成后再次运行默认构建和测试，确认消融宏没有改变普通构建。

## 交付物

- 编译期消融开关
- 隔离的变体构建目标
- 自动运行和采集脚本
- 两个内核的原始实验日志
- `raw.csv` 与 `summary.csv`
- 包含方法、环境、结果、限制和结论的 `report.md`
- README 中的复现实验命令

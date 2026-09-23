# Stage 02 消融实验报告

## 环境

- 平台：`Linux-7.0.0-31-generic-x86_64-with-glibc2.39`
- Python：`3.12.3`
- primary 内核：`/boot/vmlinuz-6.16.0`，13837312 bytes，SHA-256 `c62176c98b661daa8435e485aaa73b64b1f339dfdfe809d5dd6d59515f047e2c`
- compat 内核：`/boot/vmlinuz-6.18.0.bak`，16236736 bytes，SHA-256 `ea464032aed2962f448b10752ffae41cb9aaadbf503c695afa481de1dca905c7`

## 方法

每个变体和内核执行一次功能实验，单次超时 15 秒。主内核中达到可识别内核终点的变体额外执行 10 次性能实验。
每个二进制只启用一个编译期消融宏，构建目录相互隔离。

## 功能结果

| 内核 | 变体 | 结果 | Linux 日志 | e820 | 串口输出 | KVM exits | 超时 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| primary | baseline | halted | 1 | 1 | 1 | 209384 | 0 |
| primary | no_cpuid | halted | 0 | 0 | 0 | 1 | 0 |
| primary | fixed_32m | loader_rejected | 0 | 0 | 0 | 0 | 0 |
| primary | no_boot_params | triple_fault | 0 | 0 | 0 | 1 | 0 |
| primary | no_e820 | panic | 1 | 0 | 1 | 未记录 | 1 |
| primary | no_cmdline | halted | 0 | 0 | 0 | 179477 | 0 |
| primary | no_protected_mode | timeout | 0 | 0 | 0 | 未记录 | 1 |
| primary | no_uart | timeout | 0 | 0 | 0 | 未记录 | 1 |
| compat | baseline | panic | 1 | 1 | 1 | 未记录 | 1 |
| compat | no_cpuid | halted | 0 | 0 | 0 | 1 | 0 |
| compat | fixed_32m | loader_rejected | 0 | 0 | 0 | 0 | 0 |
| compat | no_boot_params | triple_fault | 0 | 0 | 0 | 1 | 0 |
| compat | no_e820 | panic | 1 | 0 | 1 | 未记录 | 1 |
| compat | no_cmdline | timeout | 0 | 0 | 1 | 未记录 | 1 |
| compat | no_protected_mode | timeout | 0 | 0 | 0 | 未记录 | 1 |
| compat | no_uart | timeout | 0 | 0 | 0 | 未记录 | 1 |

## 性能结果

| 变体 | 次数 | 终点识别率 | 时间均值 ms | 时间范围 ms | exits 均值 | RSS 均值 KiB |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| baseline | 10 | 1.000 | 2160.000 | 1820–2490 | 209384.000 | 91212.800 |
| no_e820 | 10 | 1.000 | 15002.000 | 15002–15002 |  |  |

## 逐项解释

- `baseline`：结果为 `halted`。
- `no_cpuid`：结果为 `halted`，没有串口证据证明内核到达 Linux version。
- `fixed_32m`：结果为 `loader_rejected`，没有串口证据证明内核到达 Linux version。
- `no_boot_params`：结果为 `triple_fault`，没有串口证据证明内核到达 Linux version。
- `no_e820`：结果为 `panic`。
- `no_cmdline`：结果为 `halted`，没有串口证据证明内核到达 Linux version；缺少串口输出不等同于客户机停止。
- `no_protected_mode`：结果为 `timeout`，没有串口证据证明内核到达 Linux version。
- `no_uart`：结果为 `timeout`，没有串口证据证明内核到达 Linux version；缺少串口输出不等同于客户机停止。

## 限制

- 结果只适用于记录的内核、宿主机 KVM 和当前 Stage 02 实现。
- 超时表示观察窗口内没有终止，不能单独证明客户机崩溃。
- 无 Linux 串口日志的变体只能依据宿主机 KVM exit 判断进度。
- 超时终止的进程无法输出最终 exit 计数，且 `/usr/bin/time` 可能无法写入 RSS；这些不可得值留空。
- 最大 RSS 包含 VMM 进程及 `/usr/bin/time` 观测到的宿主机开销。

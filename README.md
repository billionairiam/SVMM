# Stage 01：COM1 串口控制台

根据 [Runra 的 Stage 01 教程](https://runra.dev/zh/blog/sandbox-from-scratch-01-serial-console) 实现的最小 x86 KVM 示例。客户机运行一段 16 位实模式代码，逐字节写 COM1 端口 `0x3f8`；宿主机在 `KVM_EXIT_IO` 时将数据分发给独立的串口设备模型，并写到标准输出。

## 使用

需要 Linux x86_64、C 编译器、内核 KVM 接口，以及当前用户对 `/dev/kvm` 的读写权限。

```sh
make all
make run
make test
```

`make run` 的标准输出应为：

```text
hello from guest
```

`make test` 总会编译并运行串口单元测试；若 `/dev/kvm` 不可用，则跳过虚拟机运行测试。

## 结构与边界

- `src/kvm.c`：打开 KVM 并创建 VM。
- `src/memory.c`：分配并注册 64 KiB 客户机内存。
- `src/vcpu.c`：配置实模式 vCPU，运行并分发 I/O 退出。
- `src/serial.c`：处理 COM1 的 8 个端口；仅数据端口写入会输出，其余写入被忽略。
- `src/guest_code.h`：输出 `hello from guest` 的实模式机器码。

这个 Stage 01 示例只支持串口输出；不实现输入、LSR、IRQ，也不启动 Linux 内核。

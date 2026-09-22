# Stage 00：第一个 KVM I/O 退出

根据 [Runra Stage 00 教程](https://runra.dev/zh/blog/sandbox-from-scratch-00-first-kvm-exit-io) 实现的最小 x86 KVM 虚拟机。客户机在 16 位实模式中执行 9 对 `mov al,字符` / `out 0xe9,al` 指令。每次 `out` 都触发一次 `KVM_EXIT_IO`，VMM 将该字节写入标准输出；最后 `hlt` 触发 `KVM_EXIT_HLT`。

## 运行

需要 Linux x86_64、C 编译器和当前用户对 `/dev/kvm` 的读写权限。

```sh
make all
make run
make test
```

客户机准确输出 9 个字节：`hello,box`，末尾**没有换行**。教程给出的 37 字节程序只有 9 次 I/O 退出，也没有发送换行字节；测试按客户机实际发送的字节比较。

`make test` 会编译，并在 KVM 可用时运行虚拟机、与 `tests/golden/stage-00.txt` 按字节比对；其他环境跳过运行测试。

## 文件

- `src/kvm.c`：打开 `/dev/kvm`、检查 API 版本、创建 VM 并设置 TSS 地址。
- `src/memory.c`：分配并注册 64 KiB 客户机内存。
- `src/guest_code.h`：37 字节客户机机器码。
- `src/vcpu.c`：创建 vCPU、设置实模式寄存器、处理 `KVM_EXIT_IO` 和 `KVM_EXIT_HLT`。
- `src/main.c`：组织初始化、运行和资源释放。

此阶段仅处理调试端口 `0xe9`，没有串口设备模型、Linux 内核或其他虚拟设备。

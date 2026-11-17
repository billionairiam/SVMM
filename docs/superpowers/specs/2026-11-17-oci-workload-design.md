# Stage 12：从 OCI 镜像运行 workload

状态：依据用户继续实现并在完成后 push 的指令进入执行；采用已讨论的 virtio-blk 路线。

## 意图与范围

借鉴 libkrun，使用容器镜像的 rootfs 作为 guest 的应用环境：一个镜像、一台 VM、一个受监督的主应用进程。应用、动态链接器和共享库来自镜像，内核和静态 guest init 由 SVMM 提供。

本期完成镜像转换、guest 初始化、应用配置和退出状态回传。完整 OCI runtime 生命周期、多容器、Compose、Kubernetes、宿主目录挂载、virtio-fs、分层缓存及跨架构模拟留到后续。不得依赖应用镜像中有 shell 或 /sbin/init，不把 VM boot 成功当作应用成功。

## 已有能力与约束

- BusyBox initramfs 的 /init 挂基础文件系统、把 /dev/vda 挂到 /mnt、配置 eth0，再启动交互 shell。
- --disk 提供一块读写 virtio-blk 磁盘，--tap 提供 virtio-net；ACPI 描述这两个设备。
- vcpu.c 捕获电源端口，sandbox_run 当前把正常 guest 关机统一返回 0。
- 冷快照保存磁盘；热快照保存 RAM、CPU、设备和同一时刻的磁盘。
- Linux x86_64、单 vCPU。guest 内核需内建 ACPI、virtio-mmio、virtio-blk、ext4、devtmpfs；网络另需 virtio-net。
- 本机 6.16 内核未启用 virtio-mmio；使用 /boot/vmlinuz-6.18.0.bak 后，现有启动关机和磁盘持久化两条 E2E 均通过。
- 保留当前 src/serial.c 的用户修改和 docs/trace-cat-hello.md。
- 遵守 AGENTS.md：先列失败方式、先写失败测试、再实现；只跑相关真实 E2E，本期不加孤立 unit test。

## 方案比较

1. **rootfs 转 ext4，经 virtio-blk 交付（推荐）**：复用已有设备、文件锁和快照。需要构建时展开并复制 rootfs，每个实例使用独立的可写磁盘。
2. **rootfs 目录经 virtio-fs 交付**：接近 libkrun 的常见容器模式，便于接容器存储。需要新增文件系统设备和后端，调整 jail 权限，另行解决目录快照一致性。
3. **VM 内 Docker Engine**：支持在 guest 中 docker run，但增加运行时、镜像管理和网络层，超出本轮直接启动应用的目标。

用户已要求按讨论实现；virtio-blk 是实现者根据上一轮推荐作出的具体选择，尚可由用户纠正。

## 接口与镜像构建

新增 scripts/build-workload.py 和 make workload-initramfs，预期用法：

```sh
make all workload-initramfs
python3 scripts/build-workload.py --image ubuntu:24.04 --output build/ubuntu.ext4 -- /bin/sh -c 'echo hello; exit 7'
bin/svmm run --kernel /path/to/bzImage --initrd build/workload-initramfs.cpio.gz --disk build/ubuntu.ext4 --workload
```

- 构建工具依赖宿主机 Docker CLI/daemon、Python 3、tar 和 e2fsprogs；guest init 用 musl-gcc 静态编译，编译器可覆盖，缺依赖明确报错。
- Docker 负责拉取和正确合并镜像层。工具固定 image ID，创建一个从不启动的临时容器并导出 rootfs，始终清理本工具创建的容器及匿名卷，不执行镜像代码。
- 默认执行 Entrypoint + Cmd；-- 后的 argv 替换 Cmd，保留 Entrypoint。提供 --entrypoint、可重复 --env KEY=VALUE、--workdir、--user 覆盖。按数组处理参数，禁止拼 shell 命令。
- 保留 Env、WorkingDir、User，拒绝非 linux/amd64 镜像及无有效命令。输出为无分区表的整盘 ext4，可指定容量，默认按文件占用加余量计算。
- 磁盘保留 /.svmm/ 放版本化启动配置和原始镜像信息，冲突时拒绝覆盖。启动配置必须无损表示空格、引号、换行，限制大小和数量，错误或截断必须失败。
- 导出、提取和打包保留 UID/GID、权限、符号/硬链接，阻止归档路径及链接写出临时工作目录。无法保留的 xattr/file capability 明确列为兼容性限制，不声称完整 OCI 兼容。
- 输出经同目录临时文件制作完成后原子发布，拒绝覆盖已存在目标；失败不留下可误用的镜像。

## Guest init 与应用生命周期

- 单独提供 workload initramfs，其中 /init 是静态 C 程序；现有 BusyBox 启动路径保持可用。
- init 挂 proc/sysfs/devtmpfs，等待并挂载 /dev/vda 为 ext4，切换进应用 rootfs，准备 /dev/pts、/dev/shm、/run、/tmp。允许保留小 initramfs 在内存，释放旧根不是本期要求。
- 网络沿用 192.168.100.2/24、网关 192.168.100.1 和现有 DNS/TAP 配置，由 init 直接配置，不依赖镜像中的 ip；无 eth0 时支持离线运行。
- init 始终为 PID 1，读取配置并 fork 主应用，应用获得指定 env/cwd/user。支持数字 UID/GID 及 rootfs passwd/group 可解析的用户名和组名，正确设置补充组，不能忽略 USER 默默以 root 执行。
- 应用 stdio 接现有串口并合流；宿主 stderr 继续输出 VMM 指标。本期不承诺独立 stdout/stderr 或完整 Docker TTY 语义。
- init 得到主应用 wait 状态、回收孤儿进程，清理剩余进程，再 sync 并尝试将 rootfs 重新挂为只读，最后报告状态。
- 正常退出映射 0–255，信号结束映射 128+signal；命令不存在 127、不可执行 126、初始化/配置错误 125。失败不进入无限 shell。
- 保留宿主 SIGTERM/Ctrl-A x 立即停止的既有行为，不声称其可优雅关闭应用；guest PID 1 收到终止信号时可转发给应用进程组。

## VMM 退出状态通道

- 新增 --workload，要求 --disk；冷启动还要求 workload initramfs。普通 run 的返回码语义不变。
- guest init 通过专用 x86 I/O 端口发送带 magic 的 32 位退出消息。端口与 magic 放在宿主和 guest 共用的协议头中；严格检查方向、宽度、count、magic 和状态范围。
- 有效消息记录 reason=workload_exit 及状态，清理 VM 并把应用状态作为宿主退出码。workload 模式下，未报告状态便 poweroff/reset/崩溃视为失败；明确的宿主停止及快照保存保留其操作成功语义。
- 不解析串口文本作控制消息。guest 特权程序能提前结束自己的 VM，退出报告不作为对恶意 guest 的可信证明。
- 字段和内部接口由实现者确定，避免改变已有 vmstate 编码格式。

## 快照与 jail

- rootfs 和规范化启动配置均位于 --disk；冷快照重启应用，热快照继续原应用。
- 恢复 workload 热快照显式使用：svmm run --restore NAME --disk PATH --workload；无需 kernel/initrd，仍检查退出状态。不自动推断快照的 workload 模式。
- 新状态消息只在 VM 结束时发生，不引入额外设备或需保存的中间协议状态。
- 镜像准备在 VMM 之外；guest mount/exec 不要求宿主 jail 放宽 seccomp。保留已有 warm-save/restore 与 jail 的互斥约束。

## 先列失败方式，再写真实 E2E

1. argv/env/cwd/user 丢失：用 FROM scratch 静态探针镜像，实测并精确比对带空格、引号、换行的参数/变量及 UID/GID；同时证明无 shell 和 /sbin/init 也可运行。
2. 退出 7 被当作成功：实际运行，验证宿主返回 7；另测信号结束、exec 失败和坏配置，必须及时退出。
3. 丢权限或链接：应用实际读链接、读不同 owner 的文件、写仅指定用户可写的工作目录。
4. 转换失败覆盖原文件或泄漏容器：预置目标内容，验证拒绝覆盖且原字节不变，检查工具创建的临时容器被清理。
5. 服务只启动但不可访问：使用独立 TAP/netns 运行 HTTP workload，宿主请求并验证响应。
6. 写入未落盘或关机损坏磁盘：应用写数据，退出后 debugfs 读回，e2fsck 验证。
7. 快照丢状态或退出码：保存运行中的计数/等待输入应用，恢复后校验原内存状态，再退出 7，宿主仍返回 7。
8. 既有行为回归：复跑普通 BusyBox 启动关机、磁盘持久化和相关 CLI E2E。

缺 Docker/KVM 等环境时默认可跳过；显式要求对应集成测试或指定内核时必须失败而非静默跳过。开发阶段只运行相关用例，不启动全项目全量测试。

## 实现者自行确定的细节

启动配置的具体编码、C helper 划分、临时目录命名、磁盘余量和超时值由实现者决定并记录。实现者在当前会话自行实现与验证，不拆成并行实现任务。

# Stage 08：网络命名空间与出口策略

根据 [Runra Stage 08 教程](https://runra.dev/zh/blog/sandbox-from-scratch-08-network-egress-policy)：KVM 只隔离 CPU 和内存，客户机经 TAP 发出的包在宿主机内核里是普通网络包，能访问云 metadata（`169.254.169.254`）、内网和宿主机自己。Stage 08 让 VMM 进程 `setns` 进一个专门的网络命名空间，客户机的流量只能经过那里的 nftables denylist 出去。

```sh
sudo scripts/setup-netns-policy.sh           # 或 sudo make netns：netns + tap0 + veth + NAT + denylist
sudo make run-netns KERNEL=/path/to/bzImage  # 等价于 svmm run ... --tap tap0 --netns sandbox-ns
# 客户机里：policy-test（2 个目标应该通，3 个应该被拒）
sudo ip netns exec sandbox-ns nft list chain inet sandbox-egress forward   # 看被拒的计数
sudo dmesg | grep SANDBOX-DENY                                             # 被拒的连接（限速记录）
sudo scripts/setup-netns-policy.sh --clean   # 或 sudo make netns-down
```

```
客户机 eth0 192.168.100.2
     │ virtio-net
┌────┴──────────────────────────── sandbox-ns ──┐
│ tap0 192.168.100.1                             │  forward 链：denylist（先计数再丢弃）
│   └─ MASQUERADE → 192.168.200.2                │  IPv6 关闭，forward 链也丢 IPv6
│ veth-g 192.168.200.2，默认路由 → 192.168.200.1   │
└────┬───────────────────────────────────────────┘
     │ veth pair
veth-h 192.168.200.1（宿主机）── MASQUERADE ── 宿主机出口
```

denylist：`169.254.0.0/16`（含 `169.254.169.254`）、`100.100.100.200`、`10.0.0.0/8`、`172.16.0.0/12`、`192.168.0.0/16`、`100.64.0.0/10`。宿主机的 `veth-h` 也在其中，客户机访问不到它。`ct state established,related` 放在 denylist 之前，只检查新连接的目标地址；外面主动连向客户机（目标 `192.168.100.2`）同样被 `192.168.0.0/16` 挡住。

**只能走代理的网络**（例如公司代理和 DNS 都在 10/8 里）：用 `ALLOW_DST` 在 denylist 上开例外，然后在客户机里走代理：

```sh
sudo ALLOW_DST="10.102.248.16 10.248.2.5" scripts/setup-netns-policy.sh
# 客户机里：
echo "nameserver 10.248.2.5" > /etc/resolv.conf
export http_proxy=http://10.102.248.16:912
wget -O - http://example.com/
POLICY_ALLOW_PING=10.248.2.5 POLICY_ALLOW_URL=http://example.com/ policy-test
```

注意：放行代理等于把代理能到达的地方都交给了客户机。设了 `http_proxy` 之后，`wget http://169.254.169.254/` 是代理替客户机去连的，nftables 只看到发往代理的包。

代码：

- `src/net-policy.{h,c}`：`net_policy_enter_ns(name)` = 校验名字 → `open("/var/run/netns/<name>")` → `setns(fd, CLONE_NEWNET)` → `close`。名字只允许 `[A-Za-z0-9_.-]`、不超过 64 个字符、不能是 `.`/`..`（否则 `../../proc/1/ns/net` 会回到宿主机的 netns）。不存在（提示 `ip netns add`）、残留的普通文件（`setns` 返回 `EINVAL`）、没有 `CAP_SYS_ADMIN` 时以 `stage=failed_netns` 失败，不会启动 VM。
- `src/sandbox.{h,c}`：`netns_name` 字段；`sandbox_run()` 的第 0 步，在读内核、打开 TAP、创建任何线程之前。TAP 在 `TUNSETIFF` 时所在的 netns 里查找，`setns` 只移动调用线程，之后创建的控制台线程和 RX 线程继承它。
- `src/cli.c`：`-n/--netns NAME`。VMM 自己 `setns`，不需要 `ip netns exec`。
- `scripts/setup-netns-policy.sh`：建 netns（先关 IPv6）、veth pair、sandbox 里的 tap0、两层 MASQUERADE、nftables 表 `inet sandbox-egress`（`deny-dst-v4`、`allow-dst-v4` 两个 interval 集合和 `forward` 链）和 `ip sandbox-nat`。宿主机上加 MASQUERADE 和 FORWARD 放行，打开 `ip_forward`；在 init netns 里运行时还会打开 `net.netfilter.nf_log_all_netns`（否则非 init netns 的 nftables 日志不进 dmesg）。原值记在 `/run/svmm/netns-<name>.state`，`--clean` 恢复。netns 或 `veth-h` 已存在时拒绝执行；netns 里还有进程（svmm 没退出）时 `--clean` 拒绝执行。环境变量 `NETNS`、`TAP`、`ALLOW_DST`（只接受 IPv4 地址/CIDR，防止注入 nft 语句）、`SVMM_NETNS_STATE_DIR`。
- `rootfs/build-rootfs.sh`：客户机里的 `/bin/policy-test`，`POLICY_ALLOW_PING`、`POLICY_ALLOW_URL` 可以换掉允许的目标。
- `Makefile`：`netns`、`netns-down`、`run-netns`、`help`。

和教程的差别：`policy-test` 不在每次启动时自动运行（被拒的连接要等超时，会拖慢每次启动），在 shell 里手动运行；没有 `run-in-netns.sh`，`--netns` 加 `make run-netns` 已经覆盖；多了 `ALLOW_DST` 例外、被拒日志限速、forward 链丢 IPv6。

局限：denylist 只挡私有/特殊地址，宿主机的公网地址仍然能访问；没有速率限制和 DNS 过滤；一次只有一个 sandbox-ns（`veth-h` 名字固定）。

测试：`tests/test_network_policy.py`（端到端，需要 root、`nft`，`BZIMAGE_PATH=... python3 -m unittest -v tests/test_network_policy.py`）。每个测试新建一个“宿主机” netns，被拒的地址都配在它上面并且有 HTTP 服务（没有策略就能连上），`ALLOW_DST` 放行的 `10.0.0.2` 作为对照；另有一个 wan netns 扮演外网，只有两层 MASQUERADE 都生效才能收到回复。宿主 netns 里还放了一个同名的诱饵 `tap0`，验证 VMM 打开的是 sandbox-ns 里的那个。

# Stage 07：virtio-net 与 TAP

根据 [Runra Stage 07 教程](https://runra.dev/zh/blog/sandbox-from-scratch-07-virtio-net-tap) 给客户机加一块网卡：`svmm run --tap tap0` 把宿主机上已有的 TAP 接口接成客户机里的 `eth0`（virtio-net），`/init` 自动配置静态地址并 ping 网关。

```sh
sudo scripts/setup-tap.sh                    # 创建 tap0 (192.168.100.1/24)，开转发和 MASQUERADE
make run-net KERNEL=/path/to/bzImage         # 等价于 svmm run ... --tap tap0（TAP=... 可改名）
sudo scripts/setup-tap.sh --clean            # 删除 tap0 和加的规则，恢复 ip_forward
```

启动后会看到：

```
virtio-net: eth0 detected
virtio-net: eth0 configured (192.168.100.2/24)
virtio-net: default gateway set to 192.168.100.1
virtio-net: DNS set to 8.8.8.8
virtio-net: testing network connectivity...
virtio-net: ping gateway 192.168.100.1 OK
virtio-net: network ready
```

`--tap` 和 `--disk` 可以同时使用。内核需要 `CONFIG_VIRTIO_NET`（加上 Stage 06 的 `CONFIG_ACPI`、`CONFIG_VIRTIO_MMIO`）。

```
客户机 eth0 192.168.100.2 ──virtio-net── tap0 192.168.100.1 ──NAT── 宿主机出口
        (GPA 0xd0001000, GSI 6)          (/dev/net/tun fd)
```

代码：

- `src/virtio/net.{h,c}`：virtio-net 设备。两个队列：0 = RX（driver 放空 buffer，device 填入收到的帧），1 = TX（driver 放要发的帧）。每个帧前面有 12 字节的 `virtio_net_hdr`（`VERSION_1` 下包含 `num_buffers`）。特性只有 `VERSION_1` 和 `MAC`（默认 `52:54:00:12:34:56`），没有任何 offload，所以帧都是算好校验和、不超过 MTU 的普通以太网帧，正好是不带 `IFF_VNET_HDR` 的 TAP 的格式。
  - **TX**（vCPU 线程，`QueueNotify=1` 触发）：校验链（全部只读、GPA 在 RAM 内、至少 12 + 14 字节），用 iovec 跳过 12 字节头，`writev()` 直接从客户机内存写到 tap，没有中间拷贝；used.len = 0。写失败计入 `tx_errors`，链照常完成。
  - **RX**（专门的 RX 线程）：`poll(tap_fd, eventfd)`；先确认 RX 队列里有空 buffer 再 `read()` 一个帧，写入全 0 的头（`num_buffers = 1`）和帧，发布 used、拉中断。没有 buffer 时帧留在宿主机内核的 tap 队列里（背压），driver 补充 buffer 的 kick 和 `DRIVER_OK` 通过 eventfd 唤醒线程。帧比 buffer 大时丢弃并计入 `rx_dropped`。
  - 退出时打印 `virtio-net: tx_packets=… rx_packets=… rx_dropped=… bad_chains=…`。
- `src/virtio/mmio.{h,c}`：传输层支持多个队列（`QueueSel` 选择，每个队列独立的地址/大小/Ready），并加了一把互斥锁：vCPU 线程（寄存器访问、TX）和 RX 线程（填 RX buffer、拉中断）都在锁内操作设备状态。新增 `virtio_mmio_run_queue()`：在锁内确认队列可用（`DRIVER_OK`、Ready、没有 `NEEDS_RESET`）后运行回调并按需发中断；backend 可选的 `driver_ok` 回调。
- `src/vcpu.{h,c}`：单个 MMIO 区间改为最多 4 个区间的表，`vcpu_mmio_register()` 拒绝重叠的区间。
- `src/boot/acpi.c`：DSDT 支持多个 virtio-mmio 设备，第 i 个叫 `\_SB.VR0i`、`_UID i`。blk 固定在 `0xd0000000`/GSI 5，net 固定在 `0xd0001000`/GSI 6（没有磁盘时也一样）。
- `src/cli.c`、`src/sandbox.c`：`-t/--tap IFNAME`。tap 在创建 VM 之前打开：不存在（不会顺手创建一个没配置的）、名字太长、被另一个进程占用、不是 TAP 时以 `stage=failed_net_open` 失败。RX 线程在 `KVM_RUN` 之前启动，在释放客户机内存之前停止。
- `scripts/setup-tap.sh`：创建 tap、配地址、`ip_forward=1`、`MASQUERADE -s 192.168.100.0/24 ! -o tap0`、FORWARD 放行。原来的 `ip_forward` 记在 `/run/svmm/tap0.state`，`--clean` 只删本脚本加的规则并恢复它；tap 已存在时拒绝执行，避免重复规则。
- `rootfs/build-rootfs.sh`：`/init` 发现 `eth0` 就配置 `192.168.100.2/24`、默认路由、`/etc/resolv.conf`，ping 网关（最多 3 次）。

一次 ping 的路径：客户机把 ICMP 帧放进 TX 队列 → 写 `QueueNotify` → `KVM_EXIT_MMIO` → vCPU 线程 `writev(tap)` → 宿主机内核回复 → tap 可读 → RX 线程 `read()` 进 RX buffer → 发布 used.idx、拉高 GSI 6 → 客户机（可能正在 HLT）被中断唤醒，virtio_net 驱动收包。

### 与教程的差异

- **12 字节头**：教程按 10 字节的 legacy 头处理。协商了 `VERSION_1` 时头固定包含 `num_buffers`，是 12 字节；按 10 字节解析，每个帧都会错开 2 字节。
- **RX 用独立线程 + 中断，不在 VM exit 后轮询**：教程在每次 VM exit 后检查 tap。客户机空闲时执行 HLT，而 irqchip 在内核里，HLT 不会退出到 VMM——客户机在等包的时候恰好没有 VM exit，包永远进不来。这里 RX 线程阻塞在 `poll()` 上，帧一到就写进 RX 队列并发中断。
- **传输层加锁**：有了第二个线程，寄存器访问（可能是复位）和 RX 写入可能同时发生，所以设备状态都在一把锁内修改；复位后 RX 线程不会再碰旧的 ring。
- **没有 RX buffer 时不读 tap**：先 peek 一个 buffer 再读帧，避免读出来再丢掉；`DRIVER_OK` 和 RX kick 唤醒线程，不用定时轮询。
- **tap 必须预先存在**：`TUNSETIFF` 对不存在的名字会新建一个 tap，打错名字时 VM 能启动但网络不通；这里直接报错并提示 `setup-tap.sh`。
- **MASQUERADE 限定源网段**，`--clean` 恢复原来的 `ip_forward`，而不是无条件关掉。
- **不做 offload**（`CSUM`、`GSO`、`MRG_RXBUF`），大帧（tap MTU 调大后）在 RX 侧丢弃并计数。
- **测试**：每个 E2E 测试在独立的网络命名空间里创建 tap，不碰宿主机的网络配置，见下文。

### 测试

- `tests/test_virtio_net.c`（`bin/test_virtio_net`）：测试扮演 driver，只通过 MMIO 寄存器和假客户机内存操作设备，用 `socketpair(SOCK_SEQPACKET)` 代替 tap（一次读写正好一个帧）。文件开头列出 17 种失败方式：设备身份/队列数、TX 头的任意切分、TX 恶意链（方向、长度、GPA 越界与回绕、成环）、写 tap 失败、RX 头和帧内容、没有 buffer 时的背压与顺序、大帧丢弃、RX 恶意链、复位与 `DRIVER_OK` 之前不写内存、`avail.idx` 跳跃与 `NEEDS_RESET`、RX 线程不依赖 kick 就能投递、tap 不存在/名字非法。
- `tests/test_virtio_net.py`：真实 KVM + 内核，每个测试一个独立的网络命名空间。文件开头列出 9 种失败方式，对应 7 个测试：`/init` 输出和 MAC、宿主机 ↔ 客户机 ping 与邻居表；双向 TCP 大流量（8 MiB `wget`、4 MiB `nc` 两个方向，比对 md5）；`setup-tap.sh` 的 NAT（用另一个命名空间扮演外网，确认源地址被改写；重复执行失败；`--clean` 删干净并恢复 `ip_forward`）；坏 tap 名/被占用的 tap 在启动前被拒绝；与 `--disk` 同时使用（`/proc/interrupts` 里两个 virtio 中断）；不带 `--tap` 时没有网卡；大帧被丢弃而正常流量不受影响。需要 root、`/dev/net/tun`、`iptables`。

```sh
make bin/test_virtio_net && ./bin/test_virtio_net
VIRTIO_BZIMAGE=/path/to/bzImage python3 -m unittest -v tests/test_virtio_net.py
```

两者都包含在 `make test` 中。

## Stage 06：virtio-blk 与磁盘

根据 [Runra Stage 06 教程](https://runra.dev/zh/blog/sandbox-from-scratch-06-virtio-blk-rootfs-disk-safety) 把 Stage 05 的 virtqueue 放进客户机内存，接上 virtio-mmio 传输层和 virtio-blk 设备：`svmm run --disk PATH` 把宿主机上的 raw 镜像变成客户机里的 `/dev/vda`，initramfs 的 `/init` 自动把它挂到 `/mnt`。

```sh
make disk                                   # build/disk.img，64 MiB ext4，已存在时不覆盖
make run-disk KERNEL=/path/to/bzImage       # 等价于 svmm run ... --disk build/disk.img
```

内核需要 `CONFIG_ACPI`、`CONFIG_VIRTIO_MMIO`、`CONFIG_VIRTIO_BLK` 和镜像里文件系统的驱动（ext4）。

代码：

- `src/virtio/mmio.{h,c}`：virtio-mmio 传输层（Virtio 1.0 §4.2，Version 2）。寄存器页在 GPA `0xd0000000`，不注册成 KVM 内存槽，客户机每次读写都 `KVM_EXIT_MMIO` 到 VMM。负责设备身份、特性协商、队列配置、`QueueNotify`（doorbell）、`InterruptStatus/ACK` 和中断线，以及只读的设备配置空间。与具体设备无关，设备通过 `struct virtio_mmio_backend` 接入。
- `src/virtio/blk.{h,c}`：virtio-blk。请求链 = 16 字节请求头（只读）+ 数据段 + 1 字节状态（可写）；支持 `T_IN`、`T_OUT`、`T_FLUSH`（`fdatasync`）、`T_GET_ID`，其他类型返回 `UNSUPP`。镜像用 `O_RDWR` 打开并加 `flock(LOCK_EX|LOCK_NB)`，退出时打印一行统计。
- `src/virtio/virtq.c`：新增 `virtq_init_shared()`，让 Stage 05 的 virtqueue 直接使用 driver 告诉我们的三块客户机内存，所有链校验逻辑原样复用。
- `src/boot/acpi.c`：带磁盘时在 DSDT 里追加 `\_SB.VR00`（`_HID "LNRO0005"`，`Memory32Fixed` 寄存器页 + 电平触发 GSI 5）。
- `src/vcpu.c`：`KVM_EXIT_MMIO` 落在设备区间内时交给设备的 `access` 回调，其余地址仍按“没有设备”处理。
- `src/cli.c`、`src/sandbox.c`：`-d/--disk`；镜像在创建 VM 之前打开，路径错误、太小（不足一个扇区）或被另一个 VMM 占用时以 `stage=failed_disk_open` 失败。
- `rootfs/build-rootfs.sh`：`/init` 发现 `/dev/vda` 就 `mount /dev/vda /mnt`（文件系统类型自动识别，不是文件系统时只提示）；shell 退出时先 `sync` 和 `umount /mnt` 再 `reboot -f`。

一次读请求的路径：客户机写好描述符和 avail ring → 写 `QueueNotify` → `KVM_EXIT_MMIO` → vCPU 线程上同步处理整个 avail ring（校验、`pread` 直接读进客户机内存、写状态字节、发布 used.idx）→ 拉高 GSI 5 → 回到客户机 → 中断处理函数读 `InterruptStatus`、写 `InterruptACK`（VMM 拉低中断线）→ 回收 used ring。

### 磁盘安全

请求链里的地址、长度、扇区号都由客户机控制。任何一项不合法时不做 I/O，只写 `IOERR` 状态并照常完成，driver 不会挂住；所有检查都在第一次 `pread/pwrite` 之前完成，不会出现做了一半的多段请求：

1. **链结构**：沿用 Stage 05 的 `virtq_walk_chain()`（越界、成环、`INDIRECT`、RO 跟在 WO 后），并对快照做后续检查，避免 double fetch。请求头必须只读且 ≥ 16 字节，拷贝一份再用；状态描述符必须可写。连状态字节都写不了的链以 `len = 0` 完成。
2. **方向**：读盘的数据段必须对 device 可写，写盘的数据段必须只读，否则 device 会写进 driver 不让写的内存。
3. **GPA 范围**：每一段 `[addr, addr + len)` 都必须完整落在客户机 RAM 里，用减法比较，回绕也会被拒绝。ring 本身在 `QueueReady=1` 时检查对齐和范围。
4. **磁盘边界**：总长度是 512 的正整数倍，且 `sector ≤ capacity` 并且 `n ≤ capacity - sector`；不计算 `sector * 512`，客户机给多大的扇区号都不会溢出。
5. **完整 I/O**：`pread/pwrite` 循环到结束，短读（例如镜像在运行中被截短）返回 `IOERR`，不会把未初始化数据当成功交给客户机。

队列层面：`avail.idx` 一次前进超过 qsz 或链头越界时，device 无法以“完成一条请求”的方式恢复，于是设置 `DEVICE_NEEDS_RESET` 并发配置变更中断，之后忽略 kick，直到 driver 复位。错误日志最多 16 条，防止客户机刷屏。

### 与教程的差异

- **设备发现走 ACPI，不走命令行或扫描**。教程让客户机 `modprobe` / 扫描设备；virtio-mmio 没有可枚举的总线，内核必须被告知设备位置。命令行参数 `virtio_mmio.device=4K@0xd0000000:5` 在本 VMM 下不可用：FADT 声明了硬件精简模式，Linux 不预分配 ISA IRQ 0–15，参数里的 `5` 被当成 Linux IRQ 号，probe 时 `request_irq()` 返回 `-EINVAL`。改为在 DSDT 里声明 `LNRO0005` 设备，Linux 经 `acpi_register_gsi()` 把 GSI 5 映射成真正的 IRQ（新版 Firecracker 也是这样做的）。
- **寄存器页在 RAM 之外**。教程在客户机 RAM 里挖一个洞并拆成两个内存槽；这里放在 `0xd0000000`（最大 3 GiB 内存之上、IOAPIC 之下，Firecracker 的 MMIO 区起点），内存槽和 e820 都不用改。
- **用中断，不轮询**。教程说本阶段不发中断；Linux 的 virtio_blk 等完成中断，不发就挂住。这里完成后若 driver 没有设置 `NO_INTERRUPT` 就置 `InterruptStatus` 并拉高电平中断线，`InterruptACK` 清零后拉低，复位也拉低；判断 `NO_INTERRUPT` 前有一个全屏障。
- **QueueNum 不合法时拒绝而不是截断**。截断后 device 按另一个大小解析 ring，下标全部错位；拒绝后 `QueueReady` 读回 0，driver 放弃设备。`QueueNumMax` 是 128。
- **特性协商更严格**：只提供 `VERSION_1`、`SEG_MAX`（qsz − 2，没有 INDIRECT 时一个请求的所有描述符要放进同一个队列）、`BLK_SIZE`、`FLUSH`；driver 选了没提供的位或没选 `VERSION_1` 时不保留 `FEATURES_OK`；之后特性冻结。
- **多段请求**：教程按单个数据段处理；Linux 会发多段 scatter/gather 请求，这里逐段校验后顺序读写。
- **镜像加锁**：两个 VM 同时挂同一个 ext4 镜像会互相覆盖元数据，第二个 `svmm` 以 `failed_disk_open` 退出。
- **ext4 而不是 ext2**，`/init` 不自动写测试文件（每次启动都改镜像会让测试结果依赖之前的运行），shell 退出时卸载 `/mnt`。
- **测试**：没有照搬教程的 shell 测试脚本，先列失败方式再写测试，见下文。

### 测试

- `tests/test_virtio_blk.c`（`bin/test_virtio_blk`）：测试扮演恶意 driver，只通过 MMIO 寄存器和一块假客户机内存操作设备，后端是临时镜像。文件开头列出 17 种失败方式：身份和配置空间、特性协商、队列配置、读写/flush/GET_ID、GPA 越界与回绕、扇区越界与溢出、方向错、长度不是整扇区、请求头/状态描述符非法、成环、`avail.idx` 跳跃与 `NEEDS_RESET`、多段请求半完成、中断拉高/ACK/`NO_INTERRUPT`/复位、非法 MMIO 访问、镜像被截短、未知类型、镜像打开与加锁。
- `tests/test_virtio_blk.py`：真实 KVM + 内核的端到端测试。宿主写好每个扇区内容不同的 raw 镜像，客户机用 `dd` 读首、中、末扇区并整盘 `md5sum`；客户机写 8 MiB 随机数据并回读，宿主按偏移比对字节、确认相邻扇区未被改动；ext4 镜像上写文件、`sync`、卸载，宿主 `debugfs` 读出、`e2fsck -fn` 干净，第二次启动还能读到；坏路径/太小/目录/被占用的镜像在启动前被拒绝；不带 `--disk` 时没有多出设备和输出。内核由 `VIRTIO_BZIMAGE`（默认 `BZIMAGE_PATH`）指定，需要 e2fsprogs。

```sh
make bin/test_virtio_blk && ./bin/test_virtio_blk
VIRTIO_BZIMAGE=/path/to/bzImage python3 -m unittest -v tests/test_virtio_blk.py
```

两者都包含在 `make test` 中。

## Stage 05：Virtqueue

根据 [Runra Stage 05 教程](https://runra.dev/zh/blog/sandbox-from-scratch-05-virtio-queue) 实现 virtio 的核心数据结构 split virtqueue（Virtio 1.0 §2.6）和一个最小的设备模型。本阶段不碰 KVM：driver 和 device 在同一个进程里，doorbell 和中断都是函数调用，先把队列语义弄清楚；Stage 06 再把队列放进客户机内存、接上 transport 和 virtio-blk 协议。`svmm` 本身的行为与 Stage 04 相同。

代码在 `src/virtio/`：

- `virtq.h` / `virtq.c`：descriptor table、available ring、used ring 及两端的私有计数器。driver 侧 `virtq_desc_fill()`、`virtq_avail_submit()`、`virtq_used_pop()`；device 侧 `virtq_avail_peek()` / `virtq_avail_consume()`、`virtq_walk_chain()`、`virtq_used_push()`；调试用 `virtq_dump()`、`virtq_dump_chain()`、`virtq_avail_count()`、`virtq_used_count()`。
- `virtio.h` / `virtio.c`：设备 = 设备类型（net/blk/console）+ 一个队列 + 客户机内存视图 + `on_notify` / `on_process` / `on_interrupt` 三个回调。`virtio_device_kick()` 依次调用它们；`virtio_gpa()` 把描述符里的 GPA 翻译成宿主指针并做越界检查。
- `demo.c`：`bin/virtq-demo`，在 64 KiB 模拟客户机内存上跑两个场景，每一步之后打印队列状态：
  1. virtio-blk 风格的读：header(RO) + 512 字节 buffer(WO)，读第 3 扇区；
  2. scatter/gather 写：header(RO) + 三段不相邻的 64 字节数据(RO)，写第 5 扇区，device 按链顺序把三段收集进磁盘。

```sh
make bin/virtq-demo && ./bin/virtq-demo    # 或 make run-virtq
```

它只依赖 C11 和 libc，不需要 `/dev/kvm`、root 或内核镜像。

### 两端互不信任

virtqueue 在共享内存里，两端都不能信任对方写的内容。实现里的检查：

- 队列大小必须是 2 的幂（2–256），槽位为 `计数器 & (qsz - 1)`；`avail.idx` / `used.idx` 是 16 位单调计数器，差值按 16 位回绕计算，跨过 65535 → 0 也正确。
- 每端只用自己的私有计数器和共享的 idx 做判断。driver 在途（已发布、未回收）的链达到 qsz 时拒绝再发布；device 只能完成“已取走、未完成”的链，所以 used ring 不会溢出。
- device 发现 `avail.idx` 一次前进超过 qsz 时报错；driver 发现 `used.idx` 超过在途数量或 `used.id` 越界时报错。
- `virtq_walk_chain()` 先把整条链校验并拷贝成快照，再对快照调用回调：下标越界、成环（visited 表）、`INDIRECT`、可写描述符之后出现只读描述符都直接拒绝，回调一次都不会被调用，设备不会做一半 I/O；之后客户机再改共享描述符表也影响不到这次处理。
- `virtio_gpa()` 用减法比较，`gpa + len` 溢出回绕也会被拒绝。
- 发布 idx 用 release 语义，读取 idx 用 acquire 语义，与多线程 VMM 中的屏障位置一致。
- `on_process` 中途出错时，已经发布的完成项仍会触发中断；driver 在 `avail.flags` 设置 `VRING_AVAIL_F_NO_INTERRUPT` 时不发中断。

### 与教程的差异

- **描述符地址是 GPA，不是宿主指针**。demo 用一个静态数组模拟客户机内存，device 访问前都经过 `virtio_gpa()`。这样输出里的地址是固定的，也更接近 Stage 06。
- **满/空判断各用各的状态**。教程里 driver 用 device 的 `last_seen_avail` 判断 avail ring 是否已满，device 也能凭空写 used ring；真实系统中两端看不到对方的私有状态，这里改为 driver 限制在途链数、device 只能完成已取走的链。
- **非法链校验更严格**：除了越界和成环，还拒绝 `INDIRECT`（教程是忽略）和“只读跟在可写之后”（规范 §2.6.4.2），并且先校验整条链再回调。
- **请求头沿用 virtio-blk 的 16 字节布局**，但和教程一样省略了链尾的 status 字节。
- **测试**：不照搬教程的 127 个单元测试和地址归一化后的 golden 文件，而是先列失败方式再写测试，见下文。
- 代码放在 `src/virtio/`，演示程序是独立的 `bin/virtq-demo`，没有替换 `svmm` 的 `main.c`。

### 测试

- `tests/test_virtq.c`（`bin/test_virtq`）：文件开头列出 11 种失败方式，每个测试对应其中一条，例如批量请求跨 16 位回绕、driver 超发、device 伪造完成、恶意链（自环、不经过链头的环、越界 next、INDIRECT、RO 跟在 WO 后、越界链头）、`avail.idx` 跳跃、伪造 used 项、GPA 溢出、中断合并与 `NO_INTERRUPT`。
- `tests/test_virtq_demo.py`：端到端运行 `bin/virtq-demo`，在不带 KVM 的环境里检查 driver 真的读到了第 3 扇区、used.len 不含只读字节、三段数据按顺序写进第 5 扇区且没有越界、每一步的 idx 和两侧计数、每个场景恰好一次中断。

两者都包含在 `make test` 中。

## Stage 04：最小 Sandbox API

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
  svmm run --kernel PATH [--initrd PATH] [--mem SIZE_MB] [--cmdline STR] [--disk PATH] [--tap IFNAME]
    -k, --kernel PATH    x86 bzImage（必需）
    -i, --initrd PATH    initramfs；不传则不加载，内核会停在挂载根文件系统
    -m, --mem SIZE_MB    客户机内存，单位 MiB，64–3072，默认 256
    -c, --cmdline STR    内核命令行，整体替换默认值
    -d, --disk PATH      raw 镜像，作为 virtio-blk（/dev/vda）；运行期间读写打开并加锁（Stage 06）
    -t, --tap IFNAME     已存在的 TAP 接口，作为 virtio-net（eth0）；用 scripts/setup-tap.sh 创建（Stage 07）
    -h, --help           打印帮助到 stdout 并返回 0
  ```

  参数错误（缺 `--kernel`、未知子命令或选项、`--mem` 不是纯十进制数或越界等）打印 `ERROR: ...` 到 stderr 并返回 2，不会创建 VM。
- 默认命令行：`console=ttyS0 earlycon=uart8250,io,0x3f8 init=/init loglevel=8 acpi_rsdp=0xe0000 reboot=acpi panic=-1`。
- stdin 是终端时切换到 raw 模式，按键原样送进串口，由客户机处理行编辑和 Ctrl-C；按 **Ctrl-A x** 退出 VMM，按 Ctrl-A Ctrl-A 发送一个 Ctrl-A。退出时恢复终端模式。stdin 也可以是管道，例如 `printf 'uname -a\npoweroff -f\n' | ./bin/svmm run -k ... -i ...`。管道输入要等 shell 提示符出现后再写，8250 驱动打开端口时会清空接收 FIFO。
- 退出路径：`poweroff -f` 写 ACPI SLP_TYP=S5（`reason=poweroff`）；在 shell 中执行 `exit`、`reboot -f` 或内核 panic（`panic=-1`）时写 0xcf9 复位（`reason=reset`）；Ctrl-A x、SIGINT、SIGTERM 或 SIGHUP 让 VMM 停止 vCPU（`reason=host_stop`）。这些都视为正常结束，返回 0，并打印 `sandbox completed`。三重故障（`reason=shutdown`）、KVM 错误和启动前的资源错误返回 1，同时打印 `SVMM_METRIC stage=failed_<步骤>`。

## 测试

```sh
make test                                          # 单元测试、virtqueue、virtio-blk 和 virtio-net 设备测试；没有内核镜像（或 root、/dev/net/tun）时跳过 KVM 测试
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

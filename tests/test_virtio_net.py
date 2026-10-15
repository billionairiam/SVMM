"""Stage 07 端到端测试：virtio-net + TAP，客户机真的能通过 tap0 收发以太网帧。

需要 Linux、可读写的 /dev/kvm、/dev/net/tun、root（创建 netns 和 tap），以及一个带
CONFIG_ACPI、CONFIG_VIRTIO_MMIO、CONFIG_VIRTIO_NET 的 bzImage
（VIRTIO_BZIMAGE，未设置时用 BZIMAGE_PATH）。条件不满足时跳过；
显式设置了内核却无法运行时判为失败。

每个测试都在一个新建的网络命名空间里创建 tap0、运行 svmm 和宿主侧的对端程序：
测试机上的真实网络、iptables 规则和 ip_forward 都不会被碰到，测试之间也互不影响。

先列出可能失败的方式，下面每个测试对应其中一条或几条：

   1. 客户机里没有 eth0：DSDT 里第二个 virtio-mmio 设备（\\_SB.VR01）写错、GSI 6
      没映射成 Linux IRQ、MMIO 分派表没把 0xd0001000 交给网卡、DeviceID 不对、
      特性协商失败、两个队列配置不起来。
   2. 链路层不通：TX 帧的 virtio_net_hdr 没剥干净或剥多了，RX 帧前面的头写错，
      宿主机看到坏帧，ARP/ping 都不通；MAC 不是配置空间里的那个。
   3. 大流量时卡住或数据错乱：RX buffer 用完后帧丢失或乱序、中断丢失、
      TX 链里多段 buffer 拼错。用 TCP 双向传 MiB 级文件并比较 md5。
   4. RX 依赖 VM exit 才处理：客户机空闲（HLT 在内核里处理，不退出）时，
      宿主主动发来的连接收不到。由宿主主动连接客户机里的 nc 覆盖。
   5. NAT：scripts/setup-tap.sh 没开转发或 MASQUERADE 没生效，客户机访问不到
      tap0 以外的网络；--clean 没把它加的东西删干净，或重复执行时规则重复。
   6. --tap 参数错误时仍然启动了 VM：接口不存在（还顺手创建了一个没配置的 tap）、
      名字太长、tap 已经被另一个 VMM 占用。
   7. virtio-blk 和 virtio-net 同时存在时互相干扰（地址或中断冲突）。
   8. 不带 --tap 时行为变化：多出网卡，或 /init 打印了网络配置。
   9. 比客户机 RX buffer 还大的帧（宿主 tap MTU 调大）让设备卡住或写越界；
      应当丢弃并计数，之后网络照常工作。
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_initramfs_shell  # noqa: E402
from test_initramfs_shell import INITRAMFS, ROOT, VMM, boot_to_shell  # noqa: E402

KERNEL = os.environ.get("VIRTIO_BZIMAGE") or test_initramfs_shell.KERNEL
SETUP_TAP = os.path.join(ROOT, "scripts", "setup-tap.sh")
HOST_IP = "192.168.100.1"
GUEST_IP = "192.168.100.2"
GUEST_MAC = "52:54:00:12:34:56"
MiB = 1024 * 1024


def setUpModule():
    explicit = os.environ.get("VIRTIO_BZIMAGE") or os.environ.get("BZIMAGE_PATH")
    problem = None
    if os.uname().sysname != "Linux":
        problem = "Linux/KVM required"
    elif not os.access("/dev/kvm", os.R_OK | os.W_OK):
        problem = "/dev/kvm is unavailable"
    elif not os.access(KERNEL, os.R_OK):
        problem = "bzImage is unreadable: " + KERNEL
    elif os.geteuid() != 0:
        problem = "root required (network namespaces and tap devices)"
    elif not os.path.exists("/dev/net/tun"):
        problem = "/dev/net/tun is unavailable (modprobe tun)"
    elif not (shutil.which("ip") and shutil.which("iptables") and shutil.which("ping")):
        problem = "iproute2, iptables and ping required"
    if problem:
        if explicit:
            raise RuntimeError(problem)
        raise unittest.SkipTest(problem)
    test_initramfs_shell.KERNEL = KERNEL
    subprocess.run(["make", "-s", "-C", ROOT, "all", "initramfs"], check=True)


class NetNS:
    """一个临时网络命名空间；测试结束时删除，里面的 tap/veth 随之销毁。"""

    count = 0

    def __init__(self, test, tag):
        NetNS.count += 1
        self.name = "svmm-%s-%d-%d" % (tag, os.getpid(), NetNS.count)
        subprocess.run(["ip", "netns", "add", self.name], check=True)
        test.addCleanup(subprocess.run, ["ip", "netns", "delete", self.name])
        self.sh("ip link set lo up")

    @property
    def prefix(self):
        return ["ip", "netns", "exec", self.name]

    def sh(self, command, check=True, env=None):
        return subprocess.run([*self.prefix, "sh", "-c", command], check=check,
                              capture_output=True, text=True, env=env)

    def popen(self, argv, **kwargs):
        return subprocess.Popen([*self.prefix, *argv], **kwargs)


def md5(data):
    return hashlib.md5(data).hexdigest()


def net_stats(stderr):
    """解析 VMM 退出时打印的 `virtio-net: k=v ...` 统计行。"""
    for line in stderr.decode(errors="replace").splitlines():
        if line.startswith("virtio-net: tx_packets="):
            return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
    return None


class VirtioNetTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="svmm-net-")
        self.addCleanup(shutil.rmtree, self.tmp)
        self.ns = NetNS(self, "net")

    def make_tap(self, mtu=None):
        self.ns.sh("ip tuntap add tap0 mode tap && ip addr add %s/24 dev tap0 && "
                   "ip link set tap0 up" % HOST_IP)
        if mtu:
            self.ns.sh("ip link set tap0 mtu %d" % mtu)

    def boot(self, extra=()):
        guest = boot_to_shell(self, args=["--initrd", INITRAMFS, "--tap", "tap0", *extra],
                              prefix=self.ns.prefix)
        return guest

    def shutdown(self, guest):
        guest.send(b"exit\n")
        self.assertEqual(guest.wait(30), 0)
        stats = net_stats(guest.stderr)
        self.assertIsNotNone(stats, guest.stderr.decode(errors="replace"))
        return stats

    def serve_http(self, ns, bind, port, directory):
        log = open(os.path.join(self.tmp, "http-%s.log" % port), "w+b")
        self.addCleanup(log.close)
        proc = ns.popen([sys.executable, "-m", "http.server", str(port), "--bind", bind,
                         "--directory", directory],
                        stdout=subprocess.DEVNULL, stderr=log)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)
        return log

    # 1 2 8
    def test_guest_configures_eth0_and_reaches_host(self):
        self.make_tap()
        guest = self.boot()
        log = guest.output.decode(errors="replace")
        for line in ("virtio-net: eth0 detected",
                     "virtio-net: eth0 configured (%s/24)" % GUEST_IP,
                     "virtio-net: default gateway set to %s" % HOST_IP,
                     "virtio-net: ping gateway %s OK" % HOST_IP,
                     "virtio-net: network ready"):
            self.assertIn(line, log)
        self.assertIn("virtio-net tap0", guest.stderr.decode(errors="replace"))

        out = guest.run(b"cat /sys/class/net/eth0/address; echo MAC_$((6*7))", b"MAC_42")
        self.assertIn(GUEST_MAC, out.decode(errors="replace"))
        # 宿主机看到的是同一个 MAC：帧头没有被 virtio_net_hdr 挤歪。
        ping = self.ns.sh("ping -c 3 -W 2 %s" % GUEST_IP, check=False)
        self.assertEqual(ping.returncode, 0, ping.stdout + ping.stderr)
        neigh = self.ns.sh("ip neigh show %s dev tap0" % GUEST_IP).stdout
        self.assertIn(GUEST_MAC, neigh)

        stats = self.shutdown(guest)
        self.assertGreater(stats["tx_packets"], 0)
        self.assertGreater(stats["rx_packets"], 0)
        self.assertEqual(stats["bad_chains"], 0)
        self.assertEqual(stats["tx_errors"], 0)

    # 3 4
    def test_bulk_tcp_both_directions(self):
        self.make_tap()
        down = os.urandom(8 * MiB)
        with open(os.path.join(self.tmp, "blob"), "wb") as f:
            f.write(down)
        self.serve_http(self.ns, HOST_IP, 8000, self.tmp)
        guest = self.boot()

        # 宿主 → 客户机（客户机发起）：RX 方向的大流量，buffer 会反复用完再补充。
        out = guest.run(b"wget -q -O /tmp/blob http://%s:8000/blob && md5sum /tmp/blob; "
                        b"echo DL_$((6*7))" % HOST_IP.encode(), b"DL_42")
        self.assertIn(md5(down), out.decode(errors="replace"))

        # 客户机 → 宿主（宿主发起）：客户机先空闲地等在 accept() 里，
        # 连接请求只能靠 RX 线程 + 中断送进去。
        up = os.urandom(4 * MiB)
        guest.send(b"nc -l -p 9000 > /tmp/up; md5sum /tmp/up; echo UP_$((6*7))\n")
        time.sleep(1)
        sender = self.ns.popen(
            [sys.executable, "-c",
             "import socket,sys,time\n"
             "data=sys.stdin.buffer.read()\n"
             "for _ in range(100):\n"
             "    try:\n"
             "        s=socket.create_connection(('%s',9000),timeout=2); break\n"
             "    except OSError: time.sleep(0.2)\n"
             "s.sendall(data); s.shutdown(socket.SHUT_WR); s.recv(1); s.close()\n" % GUEST_IP],
            stdin=subprocess.PIPE)
        sender.communicate(up, timeout=60)
        self.assertEqual(sender.returncode, 0)
        out = guest.expect(b"UP_42", 60)
        self.assertIn(md5(up), out.decode(errors="replace"))

        # 客户机 → 宿主（客户机发起）：TX 方向的大流量。
        recv_path = os.path.join(self.tmp, "recv")
        receiver = self.ns.popen(
            [sys.executable, "-c",
             "import socket\n"
             "l=socket.socket(); l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
             "l.bind(('%s',9001)); l.listen(1); c,_=l.accept()\n"
             "f=open('%s','wb')\n"
             "while True:\n"
             "    b=c.recv(65536)\n"
             "    if not b: break\n"
             "    f.write(b)\n"
             "f.close()\n" % (HOST_IP, recv_path)])
        time.sleep(0.5)
        guest.run(b"nc %s 9001 < /tmp/up; echo TX_$((6*7))" % HOST_IP.encode(), b"TX_42")
        receiver.wait(30)
        with open(recv_path, "rb") as f:
            self.assertEqual(md5(f.read()), md5(up))

        stats = self.shutdown(guest)
        self.assertGreater(stats["rx_bytes"], 8 * MiB)
        self.assertGreater(stats["tx_bytes"], 4 * MiB)
        self.assertEqual(stats["bad_chains"], 0)

    # 5
    def test_nat_via_setup_script(self):
        # wan 命名空间扮演“外网”：只认识 10.99.0.0/24，没有回 192.168.100.0/24 的路由，
        # 客户机的包只有经过 MASQUERADE 改成 10.99.0.1 才能收到回复。
        wan = NetNS(self, "wan")
        self.ns.sh("ip link add uplink type veth peer name wan0 netns %s && "
                   "ip addr add 10.99.0.1/24 dev uplink && ip link set uplink up" % wan.name)
        wan.sh("ip addr add 10.99.0.2/24 dev wan0 && ip link set wan0 up")
        with open(os.path.join(self.tmp, "hello.txt"), "w") as f:
            f.write("hello from wan\n")
        http_log = self.serve_http(wan, "10.99.0.2", 8080, self.tmp)

        env = dict(os.environ, SVMM_TAP_STATE_DIR=self.tmp)
        self.ns.sh("sysctl -qw net.ipv4.ip_forward=0")
        setup = self.ns.sh(SETUP_TAP, env=env)
        self.assertIn("tap0", setup.stdout + setup.stderr)
        again = self.ns.sh(SETUP_TAP, check=False, env=env)
        self.assertNotEqual(again.returncode, 0)
        nat = self.ns.sh("iptables -t nat -S POSTROUTING").stdout
        self.assertEqual(nat.count("MASQUERADE"), 1, nat)
        self.assertEqual(self.ns.sh("sysctl -n net.ipv4.ip_forward").stdout.strip(), "1")
        self.assertIn("192.168.100.1/24", self.ns.sh("ip addr show tap0").stdout)

        guest = self.boot()
        out = guest.run(b"wget -q -O - http://10.99.0.2:8080/hello.txt; echo WAN_$((6*7))",
                        b"WAN_42")
        self.assertIn("hello from wan", out.decode(errors="replace"))
        self.shutdown(guest)
        http_log.seek(0)
        self.assertIn(b"10.99.0.1 ", http_log.read())  # 源地址已被改写

        self.ns.sh(SETUP_TAP + " --clean", env=env)
        self.assertNotEqual(self.ns.sh("ip link show tap0", check=False).returncode, 0)
        self.assertNotIn("MASQUERADE", self.ns.sh("iptables -t nat -S").stdout)
        self.assertNotIn("tap0", self.ns.sh("iptables -S FORWARD").stdout)
        self.assertEqual(self.ns.sh("sysctl -n net.ipv4.ip_forward").stdout.strip(), "0")

    # 6
    def test_bad_tap_rejected_before_boot(self):
        for name, message in ((b"nosuch0", b"does not exist"),
                              (b"x" * 20, b"too long")):
            proc = subprocess.run(
                [*self.ns.prefix, VMM, "run", "--kernel", KERNEL, "--initrd", INITRAMFS,
                 "--tap", name], cwd=ROOT, capture_output=True, timeout=30)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn(b"stage=failed_net_open", proc.stderr)
            self.assertIn(message, proc.stderr)
            self.assertNotIn(b"stage=kvm_run", proc.stderr)
        # 打错的名字不会变成一个新建的、没有配置的 tap。
        self.assertNotEqual(self.ns.sh("ip link show nosuch0", check=False).returncode, 0)

        self.make_tap()
        first = self.boot()
        second = subprocess.run(
            [*self.ns.prefix, VMM, "run", "--kernel", KERNEL, "--initrd", INITRAMFS,
             "--tap", "tap0"], cwd=ROOT, capture_output=True, timeout=30)
        self.assertNotEqual(second.returncode, 0)
        self.assertIn(b"stage=failed_net_open", second.stderr)
        self.assertIn(b"busy", second.stderr)
        # 第一台 VM 不受影响。
        first.run(b"ping -c 1 -W 2 %s >/dev/null && echo PING_$((6*7))" % HOST_IP.encode(),
                  b"PING_42")
        self.shutdown(first)

    # 7
    def test_disk_and_network_together(self):
        self.make_tap()
        disk = os.path.join(self.tmp, "raw.img")
        with open(disk, "wb") as f:
            f.write(b"svmm-disk-marker".ljust(4096, b"\0"))
        guest = self.boot(["--disk", disk])
        out = guest.run(b"head -c 16 /dev/vda; echo; ping -c 2 -W 2 %s >/dev/null && "
                        b"echo PING_$((6*7))" % HOST_IP.encode(), b"PING_42")
        self.assertIn(b"svmm-disk-marker", out)
        irqs = guest.run(b"grep virtio /proc/interrupts; echo IRQ_$((6*7))", b"IRQ_42")
        self.assertEqual(len(re.findall(rb"virtio\d", irqs)), 2, irqs)
        stats = self.shutdown(guest)
        self.assertGreater(stats["rx_packets"], 0)
        self.assertIn(b"virtio-blk: requests=", guest.stderr)

    # 8
    def test_without_tap_there_is_no_network(self):
        guest = boot_to_shell(self, prefix=self.ns.prefix)
        self.assertNotIn(b"virtio-net", guest.output)
        out = guest.run(b"ls /sys/class/net; echo NET_$((6*7))", b"NET_42")
        self.assertNotIn(b"eth0", out)
        guest.send(b"exit\n")
        self.assertEqual(guest.wait(30), 0)
        self.assertIsNone(net_stats(guest.stderr))

    # 9
    def test_oversized_frames_are_dropped(self):
        self.make_tap(mtu=9000)
        guest = self.boot()
        big = self.ns.sh("ping -c 3 -W 1 -s 4000 %s" % GUEST_IP, check=False)
        self.assertNotEqual(big.returncode, 0)
        small = self.ns.sh("ping -c 2 -W 2 %s" % GUEST_IP, check=False)
        self.assertEqual(small.returncode, 0, small.stdout + small.stderr)
        stats = self.shutdown(guest)
        self.assertGreaterEqual(stats["rx_dropped"], 3)
        self.assertEqual(stats["bad_chains"], 0)


if __name__ == "__main__":
    unittest.main()

"""Stage 08 端到端测试：VMM 进入独立 netns，客户机的出口流量受 nftables denylist 约束。

需要的环境和 tests/test_virtio_net.py 一样（KVM、bzImage、root、/dev/net/tun、iproute2、
iptables），另外需要 nft。条件不满足时跳过；显式设置了内核却无法运行时判为失败。

每个测试先建一个临时 netns 扮演“宿主机”（下面叫 host），在里面运行
scripts/setup-netns-policy.sh 和 svmm；脚本再建出 sandbox netns。测试机真实的网络、
防火墙规则和 ip_forward 都不会被碰到。

   wan netns（“互联网”）       host netns（测试建的）                    sandbox netns
   198.51.100.2 wan0 ───veth─── uplink 198.51.100.1                    （脚本建的）
   http :8080                   lab0（dummy）: 10.0.0.1 10.0.0.2         tap0 192.168.100.1
                                  172.16.0.1 100.64.0.1 169.254.169.254   veth-g 192.168.200.2
                                http :80 绑在 0.0.0.0                     nftables denylist
                                veth-h 192.168.200.1 ──────────veth────── ┘
                                tap0（诱饵，没人 attach）                    │
                                                                    客户机 eth0 192.168.100.2

被拒绝的地址全都配在 host 上并且有 HTTP 服务：没有策略时它们是通的，连不上只能是
策略丢的包。ALLOW_DST 放行的 10.0.0.2 作为对照组，证明同一条路径本来能通。

先列出可能失败的方式，下面每个测试对应其中一条或几条：

   1. setns 太晚或者没生效：TAP 在 host netns 里打开（attach 到同名的诱饵 tap0，或者
      因为 host 里没有 tap0 而失败），客户机流量完全绕过 sandbox netns 和它的规则。
   2. --netns 的名字被拼进路径时逃出 /var/run/netns（"../../proc/1/ns/net" 会进入
      init netns）；不存在的 netns、残留的普通文件、没有 CAP_SYS_ADMIN 时仍然启动了
      VM，或者错误信息没有给出原因。
   3. denylist 没拦住：metadata（169.254.169.254）、RFC 1918、CGNAT、host 自己的 veth-h
      地址仍然能访问。
   4. denylist 拦多了或 NAT 断了：允许的外网地址不通；已建立连接的回包被丢；两层
      MASQUERADE（sandbox 内 192.168.100.0/24 → 192.168.200.2，host 上
      192.168.200.0/24 → 出口地址）少了一层，外网收不到或回不来。
   5. 被拒绝的连接没有留下痕迹（drop 规则没计数），无法审计。
   6. ALLOW_DST 例外不生效，或者把整个网段都放开了。
   7. IPv6 没关：sandbox netns 里 IPv6 可用，绕过只覆盖 IPv4 的 denylist。
   8. 脚本的生命周期：重复执行时规则重复；--clean 没删干净（netns、veth、host 上的
      MASQUERADE/FORWARD 规则）或没恢复 ip_forward；对已经清理过的环境再 --clean 报错。
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_initramfs_shell  # noqa: E402
import test_virtio_net  # noqa: E402
from test_initramfs_shell import INITRAMFS, ROOT, VMM, boot_to_shell  # noqa: E402
from test_virtio_net import NetNS  # noqa: E402

SETUP = os.path.join(ROOT, "scripts", "setup-netns-policy.sh")
GUEST_MAC = test_virtio_net.GUEST_MAC
WAN_IP = "198.51.100.2"
UPLINK_IP = "198.51.100.1"
# 配在 host 的 dummy 接口上，全部属于 denylist（10.0.0.2 由 ALLOW_DST 放行作对照）。
LAB_IPS = ("10.0.0.1", "10.0.0.2", "172.16.0.1", "100.64.0.1", "169.254.169.254")
ALLOWED_PRIVATE = "10.0.0.2"


def setUpModule():
    test_virtio_net.setUpModule()
    if not shutil.which("nft"):
        if os.environ.get("VIRTIO_BZIMAGE") or os.environ.get("BZIMAGE_PATH"):
            raise RuntimeError("nft (nftables) required")
        raise unittest.SkipTest("nft (nftables) required")


class NetworkPolicyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="svmm-pol-")
        self.addCleanup(shutil.rmtree, self.tmp)
        self.host = NetNS(self, "host")
        self.sandbox = "svmm-sbx-%d-%d" % (os.getpid(), NetNS.count)
        self.env = dict(os.environ, NETNS=self.sandbox, SVMM_NETNS_STATE_DIR=self.tmp)
        # 脚本失败或测试中途出错时也要删掉 sandbox netns（它在全局的 /var/run/netns 里）。
        self.addCleanup(subprocess.run, ["ip", "netns", "delete", self.sandbox],
                        stderr=subprocess.DEVNULL)

    def setup_policy(self, allow=None, check=True):
        env = dict(self.env)
        if allow is not None:
            env["ALLOW_DST"] = allow
        return self.host_script([SETUP], check, env)

    def clean_policy(self):
        return self.host_script([SETUP, "--clean"], True, self.env)

    def host_script(self, argv, check, env):
        # 不能用 ip netns exec：它给命令一个新的 slave 挂载命名空间，脚本里
        # `ip netns add` 的 bind mount 传不回来，脚本一退出 sandbox netns 就没了。
        # nsenter --net 只换网络命名空间。
        return subprocess.run(["nsenter", "--net=/var/run/netns/" + self.host.name, *argv],
                              check=check, capture_output=True, text=True, env=env)

    def in_sandbox(self, command, check=True):
        return subprocess.run(["ip", "netns", "exec", self.sandbox, "sh", "-c", command],
                              check=check, capture_output=True, text=True)

    def build_lab(self):
        """wan netns + host 上的 dummy 地址和 HTTP 服务，见模块注释里的拓扑图。"""
        wan = NetNS(self, "wan")
        self.host.sh("ip link add uplink type veth peer name wan0 netns %s && "
                     "ip addr add %s/24 dev uplink && ip link set uplink up"
                     % (wan.name, UPLINK_IP))
        # wan 没有回 192.168.x.x 的路由：只有 host 上的 MASQUERADE 生效时才回得来。
        wan.sh("ip addr add %s/24 dev wan0 && ip link set wan0 up" % WAN_IP)
        self.host.sh("ip link add lab0 type dummy && ip link set lab0 up && " +
                     " && ".join("ip addr add %s/32 dev lab0" % ip for ip in LAB_IPS))
        site = os.path.join(self.tmp, "site")
        os.mkdir(site)
        with open(os.path.join(site, "hello.txt"), "w") as f:
            f.write("hello from outside\n")
        test = test_virtio_net.VirtioNetTest.serve_http
        wan_log = test(self, wan, WAN_IP, 8080, site)
        test(self, self.host, "0.0.0.0", 80, site)
        return wan_log

    def boot(self, extra=()):
        return boot_to_shell(self, args=["--initrd", INITRAMFS, "--tap", "tap0",
                                         "--netns", self.sandbox, *extra],
                             prefix=self.host.prefix)

    def run_vmm(self, *args, prefix=None):
        if prefix is None:
            prefix = self.host.prefix
        return subprocess.run([*prefix, VMM, "run", "--kernel", test_virtio_net.KERNEL,
                               "--initrd", INITRAMFS, *args],
                              cwd=ROOT, capture_output=True, timeout=30)

    def deny_counter(self):
        chain = self.in_sandbox("nft list chain inet sandbox-egress forward").stdout
        for line in chain.splitlines():
            if "@deny-dst-v4" in line and "drop" in line:
                return int(re.search(r"counter packets (\d+)", line).group(1))
        self.fail("no counted drop rule for @deny-dst-v4:\n" + chain)

    # 1 3 4 5 6
    def test_guest_egress_follows_policy(self):
        wan_log = self.build_lab()
        # 诱饵：host 里也有一个 tap0。VMM 如果没先进 sandbox netns，就会 attach 到它。
        self.host.sh("ip tuntap add tap0 mode tap && ip link set tap0 up")
        self.setup_policy(allow=ALLOWED_PRIVATE)

        guest = self.boot()
        self.assertIn(b"INFO: entered network namespace " + self.sandbox.encode(),
                      guest.stderr)
        self.assertIn(b"virtio-net: ping gateway 192.168.100.1 OK", guest.output)
        # 有进程 attach 的 tap 才有 carrier。
        self.assertIn("NO-CARRIER", self.host.sh("ip -o link show tap0").stdout)
        self.assertNotIn("NO-CARRIER", self.in_sandbox("ip -o link show tap0").stdout)
        self.assertIn(GUEST_MAC, self.in_sandbox("ip neigh show dev tap0").stdout)

        # 客户机里的 policy-test：2 个应该通（外网 ping 和 HTTP），3 个应该被拒。
        guest.send(b"POLICY_ALLOW_PING=%s POLICY_ALLOW_URL=http://%s:8080/hello.txt "
                   b"policy-test; echo POL_$((6*7))\n" % (WAN_IP.encode(), WAN_IP.encode()))
        out = guest.expect(b"POL_42", 90).decode(errors="replace")
        self.assertIn("policy-test: passed=5 failed=0", out)
        self.assertIn("policy-test: all egress tests passed", out)

        # 更多被拒的目标：RFC 1918 172.16/12，以及 host 自己的 veth-h 地址。
        # 对照组：ALLOW_DST 放行的 10.0.0.2 在同一台 host、同一个 HTTP 服务上，能通。
        guest.send(b"for u in 172.16.0.1 192.168.200.1 %s; do "
                   b"if wget -q -T 3 -O - http://$u/hello.txt >/dev/null 2>&1; "
                   b"then echo \"R $u open\"; else echo \"R $u closed\"; fi; done; "
                   b"wget -q -T 3 -O - http://%s/hello.txt; echo PROBE_$((6*7))\n"
                   % (ALLOWED_PRIVATE.encode(), ALLOWED_PRIVATE.encode()))
        out = guest.expect(b"PROBE_42", 60).decode(errors="replace")
        self.assertIn("R 172.16.0.1 closed", out)
        self.assertIn("R 192.168.200.1 closed", out)
        self.assertIn("R %s open" % ALLOWED_PRIVATE, out)
        self.assertIn("hello from outside", out)
        # 启动时客户机只和 tap0（本机）通信，计数全部来自上面这些被拒的连接。
        self.assertGreaterEqual(self.deny_counter(), 5)

        stats = test_virtio_net.VirtioNetTest.shutdown(self, guest)
        self.assertEqual(stats["bad_chains"], 0)
        # 外网看到的源地址是 host 出口地址：两层 MASQUERADE 都生效了。
        wan_log.seek(0)
        self.assertIn(UPLINK_IP.encode() + b" ", wan_log.read())

    # 1 2
    def test_bad_netns_rejected_before_boot(self):
        self.setup_policy()
        stale = "svmm-stale-%d" % os.getpid()
        stale_path = os.path.join("/var/run/netns", stale)
        open(stale_path, "w").close()  # 不是 netns 的普通文件
        self.addCleanup(os.unlink, stale_path)

        cases = (("../../proc/1/ns/net", b"invalid"),
                 ("a/b", b"invalid"),
                 ("..", b"invalid"),
                 ("x" * 65, b"invalid"),
                 ("svmm-nosuch-%d" % os.getpid(), b"ip netns add"),
                 (stale, b"not a network namespace"))
        for name, message in cases:
            with self.subTest(name=name):
                proc = self.run_vmm("--tap", "tap0", "--netns", name)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(b"stage=failed_netns", proc.stderr)
                self.assertIn(message, proc.stderr)
                self.assertNotIn(b"stage=kvm_run", proc.stderr)

        empty = self.run_vmm("--netns", "")
        self.assertNotEqual(empty.returncode, 0)
        self.assertIn(b"--netns requires", empty.stderr)

        # 没有 CAP_SYS_ADMIN：netns 文件可读，但 setns 被拒绝。
        nobody = self.run_vmm("--tap", "tap0", "--netns", self.sandbox,
                              prefix=[*self.host.prefix, "setpriv", "--reuid=65534",
                                      "--regid=65534", "--clear-groups"])
        self.assertNotEqual(nobody.returncode, 0)
        self.assertIn(b"stage=failed_netns", nobody.stderr)
        self.assertIn(b"CAP_SYS_ADMIN", nobody.stderr)

        # tap0 只在 sandbox netns 里：不带 --netns 就找不到，证明 tap 是在 setns 之后打开的。
        outside = self.run_vmm("--tap", "tap0")
        self.assertIn(b"stage=failed_net_open", outside.stderr)
        self.assertIn(b"does not exist", outside.stderr)

    # 7 8
    def test_setup_script_lifecycle(self):
        self.host.sh("sysctl -qw net.ipv4.ip_forward=0")
        first = self.setup_policy()
        self.assertIn(self.sandbox, first.stdout + first.stderr)
        again = self.setup_policy(check=False)
        self.assertNotEqual(again.returncode, 0)

        self.assertIn(self.sandbox, subprocess.run(["ip", "netns", "list"], check=True,
                                                   capture_output=True, text=True).stdout)
        self.assertEqual(self.host.sh("sysctl -n net.ipv4.ip_forward").stdout.strip(), "1")
        nat = self.host.sh("iptables -t nat -S POSTROUTING").stdout
        self.assertEqual(nat.count("MASQUERADE"), 1, nat)
        self.assertIn("192.168.200.1/24", self.host.sh("ip addr show veth-h").stdout)
        links = self.in_sandbox("ip -o link").stdout
        self.assertIn("tap0", links)
        self.assertIn("veth-g", links)
        # IPv6 fail-closed：sandbox netns 里没有任何 IPv6 地址，forward 链也丢 IPv6。
        self.assertEqual(self.in_sandbox("ip -6 -o addr").stdout.strip(), "")
        self.assertEqual(self.in_sandbox("sysctl -n net.ipv6.conf.all.disable_ipv6")
                         .stdout.strip(), "1")

        self.clean_policy()
        self.assertNotIn(self.sandbox, subprocess.run(["ip", "netns", "list"], check=True,
                                                      capture_output=True, text=True).stdout)
        self.assertNotEqual(self.host.sh("ip link show veth-h", check=False).returncode, 0)
        self.assertNotIn("MASQUERADE", self.host.sh("iptables -t nat -S").stdout)
        self.assertNotIn("veth-h", self.host.sh("iptables -S FORWARD").stdout)
        self.assertEqual(self.host.sh("sysctl -n net.ipv4.ip_forward").stdout.strip(), "0")
        self.clean_policy()  # 再清理一次也不报错


if __name__ == "__main__":
    unittest.main()

"""Stage 09：jailer（cgroup v2 + rlimit + capability + seccomp）端到端测试。

VMM 直接由测试进程启动（--netns 让 VMM 自己 setns，不加 ip netns exec 前缀），
所以 proc.pid 就是 VMM，可以直接看 /proc/<pid>。先列出可能失败的方式：

  过滤器/顺序把 VMM 自己弄坏
  1. 白名单漏了运行阶段的调用：磁盘读写 + fdatasync、tap 收发（RX 线程）、
     控制台输入（tty raw 模式）、Ctrl-A x / 客户机 exit / SIGTERM 三条退出路径
     中任何一条触发 SIGSYS（退出码 -31，shell 里是 159），或者卡住。
  2. capability 丢早了：--tap 需要 CAP_NET_ADMIN（TUNSETIFF）、--netns 需要
     CAP_SYS_ADMIN（setns），丢在它们前面会让 --jail 和网络选项不能一起用。
  3. 丢 capability / 装 seccomp 时漏了线程：capability 是每个线程各自的，
     seccomp 不带 TSYNC 时只管调用线程；控制台线程、RX 线程仍然是满权限。

  限制没有真正生效
  4. 只丢了 bounding 集合：root 的 CapEff/CapPrm 还是满的。
  5. 没设 NO_NEW_PRIVS、rlimit 没设或只设了软限制（进程可以自己调回去）。
  6. cgroup 只创建了目录没把进程放进去；cpu.max / memory.max / pids.max 没写；
     控制器没在父目录的 subtree_control 里启用（写 cpu.max 会 ENOENT）。
  7. cpu.max 写了但不限流：客户机死循环时 VMM 的 CPU 用量超过配额。
  8. memory.max 没有覆盖客户机内存（记在了别的 cgroup 上、或者被换出到 swap），
     客户机吃内存时不会触发 OOM。

  best-effort 与清理
  9. cgroup 不可用（不是 cgroup v2 / 只读）时直接失败；应当 WARN 后继续，
     其余三层照常生效。
 10. 进程退出后 vm-<pid> 目录永远留着；下次启动时没清理已退出 VMM 的目录；
     清理时误删了正在运行的 VMM 的 cgroup。setup-jail.sh --clean 同样如此。

  命令行
 11. 子选项不带 --jail 被静默忽略；非法数值被接受；出错时已经创建了 cgroup。
 12. 不加 --jail 时行为有任何变化。
"""

import os
import re
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_initramfs_shell  # noqa: E402
import test_virtio_net  # noqa: E402
from test_initramfs_shell import INITRAMFS, PROMPT, ROOT, VMM, boot_to_shell  # noqa: E402
from test_virtio_net import HOST_IP, NetNS  # noqa: E402

CGROUP = "/sys/fs/cgroup"
PARENT = os.path.join(CGROUP, "svmm")
SETUP = os.path.join(ROOT, "scripts", "setup-jail.sh")
SIGSYS_HINT = "VMM killed by seccomp? recent audit lines:\n"
ZERO_CAPS = ("CapInh", "CapPrm", "CapEff", "CapBnd", "CapAmb")


def setUpModule():
    test_virtio_net.setUpModule()
    problem = None
    if not os.path.exists(os.path.join(CGROUP, "cgroup.controllers")):
        problem = "cgroup v2 required at " + CGROUP
    elif not shutil.which("unshare"):
        problem = "unshare (util-linux) required"
    if problem:
        if os.environ.get("BZIMAGE_PATH"):
            raise RuntimeError(problem)
        raise unittest.SkipTest(problem)


def read(path):
    with open(path) as f:
        return f.read()


def status_fields(path):
    return dict(line.split(":\t", 1) for line in read(path).splitlines() if ":\t" in line)


def seccomp_audit_lines(pid):
    out = subprocess.run(["dmesg"], capture_output=True, text=True).stdout
    return "\n".join(l for l in out.splitlines() if "type=1326" in l and "pid=%d " % pid in l)


def dead_pid():
    proc = subprocess.Popen(["true"])
    proc.wait()
    return proc.pid


class JailerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="svmm-jail-")
        self.addCleanup(shutil.rmtree, self.tmp)

    def boot(self, *extra, tty=False, prefix=()):
        guest = boot_to_shell(self, tty=tty, args=["--initrd", INITRAMFS, *extra],
                              prefix=prefix)
        self.addCleanup(self.remove_cgroup, guest)
        return guest

    def remove_cgroup(self, guest):
        guest.close()
        path = os.path.join(PARENT, "vm-%d" % guest.proc.pid)
        if os.path.isdir(path):
            os.rmdir(path)

    def cgroup_of(self, pid):
        line = read("/proc/%d/cgroup" % pid).strip()
        self.assertTrue(line.startswith("0::"), line)
        return CGROUP + line[3:]

    def assert_clean_exit(self, guest, code):
        self.assertEqual(code, 0, SIGSYS_HINT + seccomp_audit_lines(guest.proc.pid) +
                         "\nstderr:\n" + guest.stderr.decode(errors="replace"))

    def assert_jailed(self, pid, nofile=64, nproc=32):
        tasks = os.listdir("/proc/%d/task" % pid)
        # main/vCPU、控制台线程，有 --tap 时还有 RX 线程。
        self.assertGreaterEqual(len(tasks), 2)
        for tid in tasks:
            st = status_fields("/proc/%d/task/%s/status" % (pid, tid))
            self.assertEqual(st["Seccomp"], "2", (tid, st["Name"]))
            self.assertEqual(st["NoNewPrivs"], "1", (tid, st["Name"]))
            for cap in ZERO_CAPS:
                self.assertEqual(int(st[cap], 16), 0, (tid, st["Name"], cap))
        limits = read("/proc/%d/limits" % pid)
        self.assertRegex(limits, r"Max open files\s+%d\s+%d\s" % (nofile, nofile))
        self.assertRegex(limits, r"Max processes\s+%d\s+%d\s" % (nproc, nproc))

    # 1 2 3 4 5 6
    def test_jailed_vm_with_disk_tap_and_tty(self):
        ns = NetNS(self, "jail")
        ns.sh("ip tuntap add tap0 mode tap && ip addr add %s/24 dev tap0 && "
              "ip link set tap0 up" % HOST_IP)
        disk = os.path.join(self.tmp, "raw.img")
        with open(disk, "wb") as f:
            f.write(b"svmm-disk-marker".ljust(1 << 20, b"\0"))
        guest = self.boot("--disk", disk, "--tap", "tap0", "--netns", ns.name, "--jail",
                          tty=True)
        pid = guest.proc.pid
        err = guest.stderr.decode(errors="replace")
        order = [err.find(s) for s in ("INFO: jail: cgroup applied", "INFO: jail: rlimit applied",
                                       "virtio-net tap0", "INFO: jail: capabilities dropped",
                                       "INFO: jail: seccomp installed")]
        self.assertNotIn(-1, order, err)
        self.assertEqual(order, sorted(order), err)

        self.assert_jailed(pid)
        self.assertGreaterEqual(len(os.listdir("/proc/%d/task" % pid)), 3)
        cg = self.cgroup_of(pid)
        self.assertEqual(cg, os.path.join(PARENT, "vm-%d" % pid))
        self.assertEqual(read(cg + "/cgroup.procs").split(), [str(pid)])
        self.assertEqual(read(cg + "/cpu.max").split(), ["100000", "100000"])
        self.assertEqual(int(read(cg + "/memory.max")), (256 + 128) << 20)
        self.assertEqual(read(cg + "/pids.max").strip(), "32")
        # 客户机内存记在这个 cgroup 上。
        self.assertGreater(int(read(cg + "/memory.current")), 32 << 20)

        # 磁盘：读出宿主机写的标记，写入并 fsync（pwrite64 + fdatasync）。
        out = guest.run(b"head -c 16 /dev/vda; echo; echo guest-wrote-this | "
                        b"dd of=/dev/vda bs=512 seek=8 conv=fsync 2>/dev/null && "
                        b"echo DISK_$((6*7))", b"DISK_42")
        self.assertIn(b"svmm-disk-marker", out)
        # 网络：RX 线程把回复送进客户机。
        guest.run(b"ping -c 3 -W 2 %s >/dev/null && echo PING_$((6*7))" % HOST_IP.encode(),
                  b"PING_42")
        ping = ns.sh("ping -c 2 -W 2 192.168.100.2", check=False)
        self.assertEqual(ping.returncode, 0, ping.stdout)
        # 控制台：大段输入走 tty raw 模式 + 控制台线程（远超 4 KiB 接收队列）。
        blob = b"".join(b"%04d" % i + b"x" * 60 + b"\r" for i in range(100))
        guest.send(b"cat > /tmp/blob <<'EOF'\r" + blob + b"EOF\r")
        guest.expect(PROMPT, 30)
        guest.run(b"echo SIZE_$(wc -c < /tmp/blob)", b"SIZE_6500")

        guest.send(b"\x01x")
        self.assert_clean_exit(guest, guest.wait(20))
        with open(disk, "rb") as f:
            f.seek(8 * 512)
            self.assertEqual(f.read(17), b"guest-wrote-this\n")

    # 1
    def test_guest_exit_and_sigterm_under_jail(self):
        guest = self.boot("--jail")
        guest.send(b"exit\n")
        guest.expect(b"mini sandbox shell exited")
        self.assert_clean_exit(guest, guest.wait())
        self.assertEqual(guest.metric_reason(), "reset")

        guest = self.boot("--jail")
        guest.proc.send_signal(signal.SIGTERM)
        self.assert_clean_exit(guest, guest.wait())
        self.assertEqual(guest.metric_reason(), "host_stop")

    # 5 6
    def test_custom_limits(self):
        guest = self.boot("--jail", "--cpu-max", "50000", "--memory-max", "512",
                          "--nofile", "48", "--nproc", "16")
        pid = guest.proc.pid
        self.assert_jailed(pid, nofile=48, nproc=16)
        cg = self.cgroup_of(pid)
        self.assertEqual(read(cg + "/cpu.max").split(), ["50000", "100000"])
        self.assertEqual(int(read(cg + "/memory.max")), 512 << 20)
        self.assertEqual(read(cg + "/pids.max").strip(), "16")
        guest.send(b"poweroff -f\n")
        self.assert_clean_exit(guest, guest.wait())

    # 7
    def test_cpu_max_throttles_busy_guest(self):
        guest = self.boot("--jail", "--cpu-max", "20000")
        cg = self.cgroup_of(guest.proc.pid)
        guest.run(b"(while :; do :; done) & echo BUSY_$((6*7))", b"BUSY_42")
        time.sleep(0.5)

        def stat():
            return {k: int(v) for k, v in
                    (line.split() for line in read(cg + "/cpu.stat").splitlines())}
        before, t0 = stat(), time.monotonic()
        time.sleep(3)
        after, t1 = stat(), time.monotonic()
        used = (after["usage_usec"] - before["usage_usec"]) / 1e6 / (t1 - t0)
        # 配额 20%；不限流时一个死循环的 vCPU 接近 100%。
        self.assertLess(used, 0.35, after)
        self.assertGreater(used, 0.05, after)
        self.assertGreater(after["nr_throttled"], before["nr_throttled"])
        guest.proc.send_signal(signal.SIGTERM)
        self.assert_clean_exit(guest, guest.wait())

    # 8
    def test_memory_max_kills_vmm_when_guest_fills_ram(self):
        guest = self.boot("--mem", "512", "--jail", "--memory-max", "192")
        pid = guest.proc.pid
        cg = self.cgroup_of(pid)
        guest.send(b"dd if=/dev/zero of=/fill bs=1M count=380; echo FILLED_$((6*7))\n")
        code = guest.wait(60)
        self.assertEqual(code, -signal.SIGKILL, guest.output[-500:])
        self.assertNotIn(b"FILLED_42", guest.output)
        events = dict(line.split() for line in read(cg + "/memory.events").splitlines())
        self.assertGreaterEqual(int(events["oom_kill"]), 1, events)

    # 9
    def test_cgroup_unavailable_warns_and_still_jails(self):
        # 新的挂载命名空间里用 tmpfs 盖住 /sys/fs/cgroup；unshare 和 sh 都 exec，pid 不变。
        hide = ["unshare", "-m", "sh", "-c",
                'mount -t tmpfs none /sys/fs/cgroup && exec "$@"', "sh"]
        guest = self.boot("--jail", prefix=hide)
        pid = guest.proc.pid
        err = guest.stderr.decode(errors="replace")
        self.assertIn("WARN: jail: cgroup unavailable", err)
        self.assertNotIn("INFO: jail: cgroup applied", err)
        for line in ("rlimit applied", "capabilities dropped", "seccomp installed"):
            self.assertIn("INFO: jail: " + line, err)
        self.assert_jailed(pid)
        self.assertFalse(os.path.exists(os.path.join(PARENT, "vm-%d" % pid)))
        guest.send(b"poweroff -f\n")
        self.assert_clean_exit(guest, guest.wait())

    # 10
    def test_stale_cgroups_are_swept(self):
        subprocess.run(["sh", SETUP], check=True, capture_output=True)
        stale = os.path.join(PARENT, "vm-%d" % dead_pid())
        # 名字对应一个活着的进程（测试进程自己）：不能删。
        alive = os.path.join(PARENT, "vm-%d" % os.getpid())
        for path in (stale, alive):
            os.makedirs(path, exist_ok=True)
            self.addCleanup(lambda p=path: os.path.isdir(p) and os.rmdir(p))

        guest = self.boot("--jail")
        mine = os.path.join(PARENT, "vm-%d" % guest.proc.pid)
        self.assertFalse(os.path.exists(stale))
        self.assertTrue(os.path.isdir(alive))
        self.assertTrue(os.path.isdir(mine))

        os.makedirs(stale)
        clean = subprocess.run(["sh", SETUP, "--clean"], capture_output=True, text=True)
        self.assertEqual(clean.returncode, 0, clean.stderr)
        self.assertFalse(os.path.exists(stale))
        self.assertFalse(os.path.exists(alive))
        # 正在运行的 VMM 的 cgroup 非空，--clean 不能动它。
        self.assertEqual(read(mine + "/cgroup.procs").split(), [str(guest.proc.pid)])

        guest.send(b"poweroff -f\n")
        self.assert_clean_exit(guest, guest.wait())
        # VMM 退出后自己的目录留着（进程删不掉自己所在的 cgroup），--clean 收尾。
        self.assertTrue(os.path.isdir(mine))
        subprocess.run(["sh", SETUP, "--clean"], check=True, capture_output=True)
        self.assertFalse(os.path.exists(mine))

    # 11
    def test_bad_jail_options_rejected(self):
        before = set(os.listdir(PARENT)) if os.path.isdir(PARENT) else set()
        cases = [
            ["--cpu-max", "50000"],
            ["--memory-max", "512"],
            ["--nofile", "64"],
            ["--nproc", "8"],
            ["--jail", "--cpu-max", "0"],
            ["--jail", "--cpu-max", "999"],
            ["--jail", "--cpu-max", "abc"],
            ["--jail", "--memory-max", "0"],
            ["--jail", "--memory-max", "-5"],
            ["--jail", "--nofile", "0"],
            ["--jail", "--nproc", "1x"],
            ["--jail", "--nproc"],
        ]
        for args in cases:
            result = subprocess.run([VMM, "run", "--kernel", test_virtio_net.KERNEL,
                                     "--initrd", INITRAMFS, *args],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2, (args, result.stderr))
            self.assertIn("ERROR:", result.stderr, args)
            self.assertNotIn("jail:", result.stderr, args)
        after = set(os.listdir(PARENT)) if os.path.isdir(PARENT) else set()
        self.assertEqual(after, before)
        help_text = subprocess.run([VMM, "run", "--help"], capture_output=True,
                                   text=True).stdout
        for opt in ("--jail", "--cpu-max", "--memory-max", "--nofile", "--nproc"):
            self.assertIn(opt, help_text)

    # 12
    def test_without_jail_nothing_changes(self):
        guest = self.boot()
        pid = guest.proc.pid
        self.assertNotIn(b"jail:", guest.stderr)
        st = status_fields("/proc/%d/status" % pid)
        self.assertEqual(st["Seccomp"], "0")
        self.assertNotEqual(int(st["CapEff"], 16), 0)
        self.assertNotIn("/svmm/", read("/proc/%d/cgroup" % pid))
        guest.send(b"poweroff -f\n")
        self.assertEqual(guest.wait(), 0)


if __name__ == "__main__":
    unittest.main()

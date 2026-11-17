"""Stage 11 端到端测试：热快照（run --warm-save 保存，run --restore 从暂停点继续）。

先列出可能失败的方式，下面每个测试对应其中一条或几条：

  保存
    1. vCPU 状态不一致：vCPU 停在一次 PIO/MMIO 退出上，那次访问的结果（IN 的值、
       RIP 前进）还在 KVM 内部没完成，读出的寄存器就是“半条指令”；恢复后那条
       OUT 再执行一遍（串口多出一个字节）。在客户机狂打串口时反复保存/恢复，
       逐字节比对输出。
    2. 内存丢失：漏了某些页（全零页优化写错、偏移算错），恢复后 tmpfs 文件、
       shell 变量、后台进程不见了或内核直接崩溃。memory.bin 应当是稀疏的。
    3. 内存和磁盘不是同一时刻：页缓存里还没写回的数据（没 sync）和 disk.img 对不上，
       恢复后 sync 写坏文件系统。恢复后 sync + drop_caches 再读回，退出后 e2fsck 干净。
    4. 保存失败（磁盘满）留下半个快照或临时目录，或者 VMM 直接退出/客户机卡死；
       应当报错、不留任何东西、客户机继续运行，之后还能再保存。
    5. 没有 --warm-save 时 Ctrl-A s 把客户机停了，或者把 Ctrl-A s 送进了客户机。
  恢复
    6. 时钟和定时器没恢复：sleep 永远不返回（LAPIC/TSC deadline、PIT），或者
       kvmclock 从 0 重新开始，客户机时间停住直到追上保存时的 uptime，sleep 1
       要等好几秒。
    7. 中断没恢复：串口输入没有反应（IRQ 4），virtio-blk 读写卡住（IRQ 5），
       virtio-net 收不到包（IRQ 6 / RX 线程）。
    8. 恢复不可重复：同一个快照第二次恢复的起点不同，或者恢复/运行改了快照文件
       （memory.bin 被 MAP_SHARED 写回）。
    9. 链式：恢复出来的 VM 再保存，新快照不能恢复。
  校验（fail-closed）
   10. 坏快照被接受：memory.bin / vmstate.bin 改了一个字节、截断、丢失；manifest 类型
       或内存大小不对；vmstate.bin 的校验和对得上但内容不对（未知 section、重复
       section、尾随字节、CPUID 或 TSC 频率和本机不同）。都应当退出码 1、
       stage=failed_restore、客户机一条指令都不执行、--disk 一个字节都不变。
   11. 设备不匹配：快照有磁盘但这次没给 --disk（或反过来），网卡同理。
   12. 冷热快照混用：--snapshot 用了热快照、--restore 用了冷快照。
  CLI
   13. --restore 和 --kernel/--initrd/--mem/--cmdline/--snapshot 同时给出、--warm-save
       和 --jail 同时给出、名字非法时没有报用法错误（退出码 2）；--warm-save 的名字
       已经存在时 VM 已经跑起来了才发现。
   14. list / validate / delete 不认识热快照，或者 delete 留下 memory.bin / vmstate.bin。
"""

import hashlib
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_initramfs_shell  # noqa: E402
from test_initramfs_shell import INITRAMFS, KERNEL, ROOT, VMM, Guest, boot_to_shell  # noqa: E402

MiB = 1024 * 1024
DEFAULT_CMDLINE = ("console=ttyS0 earlycon=uart8250,io,0x3f8 init=/init loglevel=8 "
                   "acpi_rsdp=0xe0000 reboot=acpi panic=-1")
CTRL_A_S = b"\x01s"
FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
TAG_CPUID = 10
TAG_TSC_KHZ = 11
TAG_REGS = 13


def setUpModule():
    subprocess.run(["make", "-s", "-C", ROOT, "all", "initramfs"], check=True)


def kvm_problem():
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        return "/dev/kvm is unavailable"
    if not os.access(KERNEL, os.R_OK):
        return "bzImage is unreadable: " + KERNEL
    if not (shutil.which("mkfs.ext4") and shutil.which("e2fsck")):
        return "e2fsprogs required"
    return None


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xffffffffffffffff
    return h


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def parse_vmstate(data):
    """返回 (version, [(tag, payload), ...])。"""
    assert data[:8] == b"SVMMVMST"
    version, count = struct.unpack_from("<II", data, 8)
    off, sections = 16, []
    for _ in range(count):
        tag, length = struct.unpack_from("<II", data, off)
        sections.append((tag, data[off + 8:off + 8 + length]))
        off += 8 + length
    assert off == len(data)
    return version, sections


def build_vmstate(version, sections, trailing=b""):
    out = b"SVMMVMST" + struct.pack("<II", version, len(sections))
    for tag, payload in sections:
        out += struct.pack("<II", tag, len(payload)) + payload
    return out + trailing


class WarmBase(unittest.TestCase):
    MEM = "128"

    def setUp(self):
        problem = kvm_problem()
        if problem:
            if os.environ.get("BZIMAGE_PATH"):
                raise RuntimeError(problem)
            self.skipTest(problem)
        test_initramfs_shell.KERNEL = KERNEL
        self.tmp = tempfile.mkdtemp(prefix="svmm-warm-")
        self.addCleanup(shutil.rmtree, self.tmp)
        self.root = os.path.join(self.tmp, "snapshots")
        old = os.environ.get("SVMM_SNAPSHOT_DIR")
        os.environ["SVMM_SNAPSHOT_DIR"] = self.root
        self.addCleanup(lambda: os.environ.__setitem__("SVMM_SNAPSHOT_DIR", old)
                        if old is not None else os.environ.pop("SVMM_SNAPSHOT_DIR"))
        self.disk = os.path.join(self.tmp, "disk.img")
        with open(self.disk, "wb") as f:
            f.truncate(64 * MiB)
        subprocess.run(["mkfs.ext4", "-q", "-F", self.disk], check=True)

    def svmm(self, *args):
        return subprocess.run([VMM, *args], stdin=subprocess.DEVNULL, capture_output=True,
                              timeout=60)

    def snap(self, name, *parts):
        return os.path.join(self.root, name, *parts)

    def manifest(self, name):
        with open(self.snap(name, "metadata.json")) as f:
            return json.load(f)

    def boot(self, *extra, save="ws", prefix=()):
        args = ["--initrd", INITRAMFS, "--mem", self.MEM, "--disk", self.disk, *extra]
        if save:
            args += ["--warm-save", save]
        return boot_to_shell(self, tty=True, args=args, prefix=prefix)

    def wait_stderr(self, guest, pattern, timeout=30):
        deadline = time.monotonic() + timeout
        while pattern not in guest.stderr:
            if time.monotonic() > deadline or guest.proc.poll() is not None:
                time.sleep(0.2)
                if pattern in guest.stderr:
                    break
                self.fail("no %r in stderr:\n%s" % (pattern, guest.stderr.decode(errors="replace")))
            time.sleep(0.05)

    def save(self, guest, name, trigger=CTRL_A_S):
        if trigger is signal.SIGUSR2:
            guest.proc.send_signal(signal.SIGUSR2)
        else:
            guest.send(trigger)
        code = guest.wait(60)
        err = guest.stderr.decode(errors="replace")
        self.assertEqual(code, 0, err)
        self.assertIn("warm snapshot '%s' saved" % name, err)
        self.assertEqual(guest.metric_reason(), "warm_save")

    def restore(self, name, *extra, disk=True, prefix=()):
        args = (["--disk", self.disk] if disk else []) + list(extra)
        guest = Guest(tty=True, base_args=["run", "--restore", name], args=args, prefix=prefix)
        self.addCleanup(guest.close)
        self.wait_stderr(guest, b"warm snapshot '%s' restored in" % name.encode())
        return guest

    def shutdown(self, guest):
        guest.send(b"exit\r")
        self.assertEqual(guest.wait(30), 0, guest.stderr.decode(errors="replace"))
        fsck = subprocess.run(["e2fsck", "-fn", self.disk], capture_output=True)
        self.assertEqual(fsck.returncode, 0, fsck.stdout)


class WarmSnapshotTest(WarmBase):
    # 1 2 3 5 6 7 8 9 14
    def test_session_resumes_from_pause_point(self):
        guest = self.boot()
        guest.run(b"X=alpha; echo tmpdata > /tmp/warm; sleep 1000 & BG=$!; "
                  b"echo unsynced > /mnt/unsynced; echo S$((1))OK", b"S1OK")
        self.save(guest, "ws")

        mem = self.snap("ws", "memory.bin")
        self.assertEqual(os.path.getsize(mem), int(self.MEM) * MiB)
        self.assertLess(os.stat(mem).st_blocks * 512, int(self.MEM) * MiB * 3 // 4)
        m = self.manifest("ws")
        self.assertEqual(m["snapshot_type"], "warm")
        self.assertEqual(m["guest_mem_mb"], int(self.MEM))
        self.assertEqual((m["disk"], m["net"]), (1, 0))
        self.assertRegex(m["memory_hash"], r"^[0-9a-f]{16}$")
        files = ("memory.bin", "vmstate.bin", "disk.img", "metadata.json")
        before = {f: sha(self.snap("ws", f)) for f in files}
        self.assertEqual(sorted(os.listdir(self.root)), ["ws"])

        # 第一次恢复：一切都在，然后改掉 X、把脏页写回磁盘。
        guest = self.restore("ws")
        err = guest.stderr.decode(errors="replace")
        self.assertIn("stage=restored", err)
        self.assertNotIn("stage=image_loaded", err)  # 没有重新 boot
        guest.send(CTRL_A_S)
        self.wait_stderr(guest, b"Ctrl-A s ignored")
        out = guest.run(b"echo X=$X; cat /tmp/warm; kill -0 $BG && echo BG_ALIVE; "
                        b"cat /mnt/unsynced; echo R$((1))OK", b"R1OK")
        self.assertIn(b"X=alpha", out)
        self.assertIn(b"tmpdata", out)
        self.assertIn(b"BG_ALIVE", out)
        self.assertIn(b"unsynced", out)
        self.assertNotIn(b"Run /init", guest.output)
        out = guest.run(b"X=beta; sync; echo 3 > /proc/sys/vm/drop_caches; cat /mnt/unsynced; "
                        b"sleep 1; echo T$((2))OK", b"T2OK")
        self.assertIn(b"unsynced", out)
        out = guest.run(b"i=0; while [ $i -lt 300 ]; do echo line$i; i=$((i+1)); done; "
                        b"echo L$((3))OK", b"L3OK")
        self.assertIn(b"line299", out)
        self.shutdown(guest)

        # 第二次恢复同一个快照：起点相同（X 又是 alpha），快照文件没被改过。
        self.assertEqual({f: sha(self.snap("ws", f)) for f in files}, before)
        guest = self.restore("ws", "--warm-save", "ws2")
        out = guest.run(b"echo X=$X; Y=two; echo R$((4))OK", b"R4OK")
        self.assertIn(b"X=alpha", out)
        self.save(guest, "ws2", trigger=signal.SIGUSR2)
        self.assertEqual({f: sha(self.snap("ws", f)) for f in files}, before)

        # 链式：从 ws2 恢复。
        guest = self.restore("ws2")
        out = guest.run(b"echo XY=$X$Y; kill -0 $BG && echo BG_ALIVE; echo R$((5))OK", b"R5OK")
        self.assertIn(b"XY=alphatwo", out)
        self.assertIn(b"BG_ALIVE", out)
        self.shutdown(guest)

        # list / validate / delete 认识热快照。
        r = self.svmm("snapshot", "list")
        lines = r.stdout.decode().splitlines()
        self.assertEqual([line.split()[0] for line in lines], ["ws", "ws2"])
        self.assertTrue(all(line.split()[3] == "warm" for line in lines), lines)
        r = self.svmm("snapshot", "validate", "--name", "ws2")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.svmm("snapshot", "delete", "--name", "ws2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(os.listdir(self.root), ["ws"])

    # 6
    def test_kvmclock_guest_time_continues(self):
        # 默认 clocksource 是 tsc（只靠 TSC MSR 恢复）；这里强制 kvm-clock，
        # 覆盖 KVM_GET/SET_CLOCK 那条路径。
        guest = self.boot("--cmdline", DEFAULT_CMDLINE + " clocksource=kvm-clock")
        out = guest.run(b"cat /sys/devices/system/clocksource/clocksource0/current_clocksource; "
                        b"sleep 4; cut -d. -f1 /proc/uptime; echo U$((1))OK", b"U1OK")
        self.assertIn(b"kvm-clock", out)
        before = int(out.split()[-1])
        self.save(guest, "ws")

        guest = self.restore("ws")
        start = time.monotonic()
        out = guest.run(b"sleep 1; cut -d. -f1 /proc/uptime; echo U$((2))OK", b"U2OK")
        self.assertLess(time.monotonic() - start, 2.5)
        self.assertGreaterEqual(int(out.split()[-1]), before)
        self.shutdown(guest)

    # 1
    def test_save_while_guest_floods_serial(self):
        lines = 60000
        guest = self.boot(save="f0")
        guest.send(b"i=0; while [ $i -lt %d ]; do echo N$i; i=$((i+1)); done; "
                   b"echo FLOOD_$((1))DONE\r" % lines)
        guest.expect(b"\nN1000\r\n", 30)
        stream = b""
        # 每轮：保存 → 恢复（并允许再存）→ 等它再打一些。没有 quiesce 时只要有一轮
        # 停在 OUT 退出上，就会多出一个字节。
        for k in range(4):
            self.save(guest, "f%d" % k, trigger=signal.SIGUSR2)
            stream += guest.output
            guest = self.restore("f%d" % k, "--warm-save", "f%d" % (k + 1))
            guest.expect(b"\n", 30)
            guest.expect(b"\n", 30)
        guest.expect(b"FLOOD_1DONE", 120)
        stream += guest.output
        self.shutdown(guest)
        self.assertNotIn(b"FLOOD_1DONE", stream[:stream.rfind(b"\nN%d\r\n" % (lines - 1))])

        text = stream.replace(b"\r", b"")
        begin = text.index(b"\nN0\n") + 1
        end = text.index(b"FLOOD_1DONE")
        expected = b"".join(b"N%d\n" % i for i in range(lines))
        got = text[begin:end]
        if got != expected:
            at = next(i for i in range(min(len(got), len(expected))) if got[i] != expected[i])
            self.fail("serial stream differs at byte %d: got %r, expected %r" % (
                at, got[at - 20:at + 20], expected[at - 20:at + 20]))

    # 4
    def test_failed_save_keeps_guest_running(self):
        if os.geteuid() != 0:
            self.skipTest("root required (tmpfs mount)")
        os.makedirs(self.root)
        subprocess.run(["mount", "-t", "tmpfs", "-o", "size=4m", "none", self.root], check=True)
        self.addCleanup(subprocess.run, ["umount", "-l", self.root])

        guest = self.boot(save="full")
        guest.run(b"Z=kept; echo B$((1))OK", b"B1OK")
        guest.send(CTRL_A_S)
        self.wait_stderr(guest, b"warm snapshot 'full' failed")
        self.assertIsNone(guest.proc.poll())
        self.assertEqual(os.listdir(self.root), [])
        out = guest.run(b"echo Z=$Z; echo B$((2))OK", b"B2OK")
        self.assertIn(b"Z=kept", out)

        subprocess.run(["mount", "-o", "remount,size=512m", self.root], check=True)
        self.save(guest, "full")
        guest = self.restore("full")
        out = guest.run(b"echo Z=$Z; echo B$((3))OK", b"B3OK")
        self.assertIn(b"Z=kept", out)
        self.shutdown(guest)


class WarmSnapshotValidationTest(WarmBase):

    def make_base(self):
        guest = self.boot(save="base")
        guest.run(b"echo base > /tmp/b; echo M$((1))OK", b"M1OK")
        self.save(guest, "base")

    def variant(self, name, mutate):
        shutil.copytree(self.snap("base"), self.snap(name))
        m = self.manifest(name)
        m["name"] = name
        with open(self.snap(name, "metadata.json"), "w") as f:
            json.dump(m, f)
        mutate(name)

    def edit_manifest(self, name, **changes):
        m = self.manifest(name)
        m.update(changes)
        with open(self.snap(name, "metadata.json"), "w") as f:
            json.dump(m, f)

    def flip(self, path, offset=None):
        with open(path, "r+b") as f:
            data = f.read()
            if offset is None:
                # memory.bin 里第一个非零字节（在 1 MiB 之后，避开 BIOS 区）。
                offset = next(i for i in range(MiB, len(data)) if data[i])
            f.seek(offset)
            f.write(bytes([data[offset] ^ 0x40]))

    def rewrite_vmstate(self, name, transform):
        """改 vmstate.bin 的内容，再把校验和、大小改对：只有内容检查能发现。"""
        path = self.snap(name, "vmstate.bin")
        with open(path, "rb") as f:
            version, sections = parse_vmstate(f.read())
        data = transform(version, sections)
        with open(path, "wb") as f:
            f.write(data)
        self.edit_manifest(name, vmstate_size_bytes=len(data),
                           vmstate_fnv1a64="%016x" % fnv1a64(data))

    # 10 11 12 14
    def test_corrupt_or_mismatched_snapshots_fail_closed(self):
        self.make_base()
        # 让 --disk 和快照里的 disk.img 不同：恢复失败前就替换了磁盘的话看得出来。
        with open(self.disk, "r+b") as f:
            f.seek(60 * MiB)
            f.write(b"DISK-MUST-NOT-CHANGE")
        r = self.svmm("snapshot", "validate", "--name", "base")
        self.assertEqual(r.returncode, 0, r.stderr)

        def truncate(file, size):
            return lambda n: os.truncate(self.snap(n, file), size)

        def remove(file):
            return lambda n: os.unlink(self.snap(n, file))

        def section_edit(tag, fn):
            def transform(version, sections):
                return build_vmstate(version, [(t, fn(p) if t == tag else p)
                                               for t, p in sections])
            return lambda n: self.rewrite_vmstate(n, transform)

        def cpuid_vendor(payload):
            # u32 nent, u32 pad, kvm_cpuid_entry2[nent]（40 字节）；第 0 项是 leaf 0，
            # ebx 在偏移 8+16。
            p = bytearray(payload)
            p[8 + 16] ^= 0x01
            return bytes(p)

        cases = {
            "mem-flip": lambda n: self.flip(self.snap(n, "memory.bin")),
            "mem-trunc": truncate("memory.bin", int(self.MEM) * MiB - 4096),
            "mem-missing": remove("memory.bin"),
            "vms-flip": lambda n: self.flip(self.snap(n, "vmstate.bin"), 100),
            "vms-trunc": truncate("vmstate.bin", 1000),
            "vms-missing": remove("vmstate.bin"),
            "type-cold": lambda n: self.edit_manifest(n, snapshot_type="cold"),
            "mem-size": lambda n: self.edit_manifest(n, guest_mem_mb=2 * int(self.MEM)),
            "unknown-tag": lambda n: self.rewrite_vmstate(
                n, lambda v, s: build_vmstate(v, s + [(999, b"\0" * 8)])),
            "dup-regs": lambda n: self.rewrite_vmstate(
                n, lambda v, s: build_vmstate(v, s + [x for x in s if x[0] == TAG_REGS])),
            "trailing": lambda n: self.rewrite_vmstate(
                n, lambda v, s: build_vmstate(v, s, trailing=b"\0")),
            "bad-version": lambda n: self.rewrite_vmstate(n, lambda v, s: build_vmstate(v + 1, s)),
            "cpuid": section_edit(TAG_CPUID, cpuid_vendor),
            "tsc-khz": section_edit(
                TAG_TSC_KHZ, lambda p: struct.pack("<I", struct.unpack("<I", p)[0] + 1000)),
        }
        disk_before = sha(self.disk)
        for case, mutate in cases.items():
            with self.subTest(case=case):
                self.variant(case, mutate)
                r = self.svmm("run", "--restore", case, "--disk", self.disk)
                err = r.stderr.decode(errors="replace")
                self.assertEqual(r.returncode, 1, err)
                self.assertIn("stage=failed_restore", err)
                self.assertIn("ERROR", err)
                self.assertNotIn("stage=kvm_run", err)
                self.assertNotIn("restored in", err)
                self.assertEqual(sha(self.disk), disk_before)
                # validate 能发现文件层面的损坏（内容检查要在 KVM 里做，见上）。
                if case not in ("cpuid", "tsc-khz"):
                    self.assertEqual(self.svmm("snapshot", "validate", "--name", case).returncode,
                                     1, case)

        # 11：设备必须一一对应。
        for args, word in ((["--restore", "base"], "virtio-blk"),
                           (["--restore", "base", "--disk", self.disk, "--tap", "nosuchtap0"],
                            "virtio-net")):
            r = self.svmm("run", *args)
            err = r.stderr.decode(errors="replace")
            self.assertEqual(r.returncode, 1, err)
            self.assertIn(word, err)
            self.assertIn("stage=failed_restore", err)
        self.assertEqual(sha(self.disk), disk_before)

        # 12：冷热混用。
        r = self.svmm("run", "--kernel", KERNEL, "--disk", self.disk, "--snapshot", "base")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"warm snapshot", r.stderr)
        r = self.svmm("snapshot", "save", "--name", "cold", "--disk", self.disk)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.svmm("run", "--restore", "cold", "--disk", self.disk)
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"cold snapshot", r.stderr)

        # 14：delete 删掉热快照的全部文件。
        r = self.svmm("snapshot", "delete", "--name", "base")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(os.path.exists(self.snap("base")))

    # 13
    def test_cli_usage_errors(self):
        usage = [
            ["run", "--restore", "x", "--kernel", KERNEL],
            ["run", "--restore", "x", "--initrd", INITRAMFS],
            ["run", "--restore", "x", "--mem", "64"],
            ["run", "--restore", "x", "--cmdline", "quiet"],
            ["run", "--restore", "x", "--snapshot", "y", "--disk", self.disk],
            ["run", "--restore", "a/b"],
            ["run", "--kernel", KERNEL, "--warm-save", ".."],
            ["run", "--kernel", KERNEL, "--warm-save", "x", "--jail"],
            ["run"],
        ]
        for args in usage:
            with self.subTest(args=args):
                r = self.svmm(*args)
                self.assertEqual(r.returncode, 2, r.stderr)
        self.assertFalse(os.path.exists(self.root))

        r = self.svmm("run", "--restore", "nothere")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"stage=failed_restore", r.stderr)

        os.makedirs(self.snap("taken"))
        r = self.svmm("run", "--kernel", KERNEL, "--initrd", INITRAMFS, "--warm-save", "taken")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"already exists", r.stderr)
        self.assertNotIn(b"stage=kvm_run", r.stderr)


def net_setup_problem():
    if os.geteuid() != 0:
        return "root required (network namespaces and tap devices)"
    if not os.path.exists("/dev/net/tun"):
        return "/dev/net/tun is unavailable"
    if not (shutil.which("ip") and shutil.which("ping")):
        return "iproute2 and ping required"
    return None


class WarmSnapshotNetTest(WarmBase):
    """7、11：virtio-net 恢复后双向通，RX 线程从保存的队列位置接着收。"""

    HOST_IP = "192.168.100.1"
    GUEST_IP = "192.168.100.2"

    def setUp(self):
        super().setUp()
        problem = net_setup_problem()
        if problem:
            self.skipTest(problem)
        self.ns = "svmm-warm-%d" % os.getpid()
        subprocess.run(["ip", "netns", "add", self.ns], check=True)
        self.addCleanup(subprocess.run, ["ip", "netns", "delete", self.ns])
        self.ns_sh("ip link set lo up && ip tuntap add tap0 mode tap && "
                   "ip addr add %s/24 dev tap0 && ip link set tap0 up" % self.HOST_IP)

    def ns_sh(self, command, check=True):
        return subprocess.run(["ip", "netns", "exec", self.ns, "sh", "-c", command],
                              check=check, capture_output=True, text=True)

    def test_network_works_after_restore(self):
        prefix = ["ip", "netns", "exec", self.ns]
        guest = self.boot("--tap", "tap0", save="net", prefix=prefix)
        self.assertIn(b"virtio-net: network ready", guest.output)
        self.save(guest, "net")
        self.assertEqual(self.manifest("net")["net"], 1)

        r = self.svmm("run", "--restore", "net", "--disk", self.disk)
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"virtio-net", r.stderr)

        guest = self.restore("net", "--tap", "tap0", prefix=prefix)
        guest.run(b"ping -c 2 -W 2 %s >/dev/null && echo PING_$((6*7))" %
                  self.HOST_IP.encode(), b"PING_42")
        ping = self.ns_sh("ping -c 3 -W 2 %s" % self.GUEST_IP, check=False)
        self.assertEqual(ping.returncode, 0, ping.stdout + ping.stderr)
        self.shutdown(guest)


if __name__ == "__main__":
    unittest.main()

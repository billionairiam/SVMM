"""Stage 06 端到端测试：virtio-blk 磁盘真的能被 Linux 识别、挂载、读写并落盘。

需要 Linux、可读写的 /dev/kvm、mkfs.ext4/debugfs，以及一个带
CONFIG_ACPI、CONFIG_VIRTIO_MMIO、CONFIG_VIRTIO_BLK、
CONFIG_EXT4_FS 的 bzImage（VIRTIO_BZIMAGE，未设置时用 BZIMAGE_PATH）。
条件不满足时跳过；显式设置了内核却无法运行时判为失败。

先列出可能失败的方式，下面每个测试对应其中一条或几条：

   1. 内核没发现设备或 probe 失败：DSDT 里没有 virtio-mmio 设备、中断没映射成
      Linux IRQ、MMIO 退出没分派到设备、寄存器身份不对、特性协商失败，
      客户机里没有 /dev/vda。
   2. capacity 不对：客户机看到的扇区数和镜像大小不一致。
   3. 读路径数据错：客户机读到的扇区内容和宿主写进镜像的不一致（包括最后一个扇区）。
   4. 写路径不落盘：客户机写了、sync 了，宿主上的镜像文件里没有，或下次启动看不到。
   5. 中断丢失或完成项错位：大块、多段 I/O 时请求挂住或数据错乱。
   6. flush 没实现或没协商：ext4 提交日志时挂住，或 VMM 统计里没有 flush。
   7. 文件系统损坏：客户机正常卸载之后 e2fsck 报错。
   8. 磁盘参数错误时仍然启动了 VM：路径不存在、镜像太小、另一个 VMM 正在使用。
   9. 不带 --disk 时行为变化：多出 virtio 设备，或 /init 输出了挂载信息。
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_initramfs_shell  # noqa: E402
from test_initramfs_shell import INITRAMFS, ROOT, VMM, Guest, boot_to_shell  # noqa: E402

KERNEL = os.environ.get("VIRTIO_BZIMAGE") or test_initramfs_shell.KERNEL
SECTOR = 512
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
    elif not (shutil.which("mkfs.ext4") and shutil.which("debugfs") and shutil.which("e2fsck")):
        problem = "e2fsprogs (mkfs.ext4, debugfs, e2fsck) required"
    if problem:
        if explicit:
            raise RuntimeError(problem)
        raise unittest.SkipTest(problem)
    # Guest 用 test_initramfs_shell.KERNEL 启动，这里换成支持 virtio 的内核。
    test_initramfs_shell.KERNEL = KERNEL
    subprocess.run(["make", "-s", "-C", ROOT, "all", "initramfs"], check=True)


def pattern(sector):
    """每个扇区内容不同，读错扇区或错位都能看出来。"""
    line = ("svmm-sector-%08d|" % sector).encode()
    return (line * (SECTOR // len(line) + 1))[:SECTOR]


class VirtioBlkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="svmm-blk-")
        self.addCleanup(shutil.rmtree, self.tmp)

    def raw_image(self, sectors):
        path = os.path.join(self.tmp, "raw.img")
        with open(path, "wb") as f:
            for s in range(sectors):
                f.write(pattern(s))
        return path

    def ext4_image(self, size_mib=64):
        path = os.path.join(self.tmp, "disk.img")
        with open(path, "wb") as f:
            f.truncate(size_mib * MiB)
        subprocess.run(["mkfs.ext4", "-q", "-F", path], check=True)
        return path

    def boot(self, disk, *extra):
        return boot_to_shell(self, args=["--initrd", INITRAMFS, "--disk", disk, *extra])

    def poweroff(self, guest):
        guest.send(b"poweroff -f\n")
        self.assertEqual(guest.wait(), 0)
        self.assertEqual(guest.metric_reason(), "poweroff")
        return guest.stderr.decode(errors="replace")

    def blk_stats(self, stderr):
        for line in stderr.splitlines():
            if line.startswith("virtio-blk: requests="):
                return dict(kv.split("=") for kv in line.split()[1:])
        self.fail("no virtio-blk stats line in stderr:\n" + stderr[-2000:])

    # 1、2、3、5
    def test_raw_disk_reads_match_host_image(self):
        sectors = 2048 + 3  # 1 MiB 多一点，最后一个扇区不在 4 KiB 页边界上
        disk = self.raw_image(sectors)
        guest = self.boot(disk)
        log = guest.output.decode(errors="replace")
        self.assertRegex(log, r"virtio_blk virtio0: \[vda\] %d 512-byte logical blocks" % sectors)
        # 不是文件系统，/init 的挂载失败但仍进入 shell。
        self.assertIn("virtio-blk: mount /dev/vda failed", log)

        guest.run(b"echo SIZE:$(cat /sys/block/vda/size):EN$((1))D", b"EN1D")
        self.assertIn(b"SIZE:%d:" % sectors, guest.output)

        for s in (0, 1, 777, sectors - 1):
            out = guest.run(b"dd if=/dev/vda bs=512 skip=%d count=1 2>/dev/null | md5sum; "
                            b"echo R$((%d+0))R" % (s, s), b"R%dR" % s)
            self.assertIn(hashlib.md5(pattern(s)).hexdigest().encode(), out, "sector %d" % s)

        # 整盘读一遍：多段、大请求，任何中断丢失都会让 dd 挂住。
        whole = hashlib.md5(b"".join(pattern(s) for s in range(sectors))).hexdigest()
        out = guest.run(b"md5sum /dev/vda; echo WH$((0))LE", b"WH0LE")
        self.assertIn(whole.encode(), out)

        # 越过末尾的读由客户机块层拒绝，不能让 VMM 出错。
        guest.run(b"dd if=/dev/vda bs=512 skip=%d count=1 2>/dev/null | wc -c; echo E$((9))ND"
                  % sectors, b"E9ND")
        stderr = self.poweroff(guest)
        stats = self.blk_stats(stderr)
        self.assertEqual(stats["errors"], "0")
        self.assertGreater(int(stats["interrupts"]), 0)

    # 4、5：大块写 + 回读，宿主上对比字节。
    def test_raw_disk_writes_reach_host_file(self):
        disk = self.raw_image(32 * 2048)  # 32 MiB
        guest = self.boot(disk, "--mem", "256")
        guest.run(b"dd if=/dev/urandom of=/tmp/r bs=1M count=8 2>/dev/null; "
                  b"dd if=/tmp/r of=/dev/vda bs=1M seek=3 conv=fsync 2>/dev/null; "
                  b"echo 3 > /proc/sys/vm/drop_caches; echo W$((1))DONE", b"W1DONE", )
        out = guest.run(b"md5sum /tmp/r; dd if=/dev/vda bs=1M skip=3 count=8 2>/dev/null | "
                        b"md5sum; echo M$((5))D", b"M5D")
        sums = [line.split()[0] for line in out.splitlines() if len(line.split()) == 2
                and len(line.split()[0]) == 32]
        self.assertEqual(len(sums), 2, out)
        self.assertEqual(sums[0], sums[1])
        # 单扇区写也要准确落在对应偏移，不能影响相邻扇区。
        guest.run(b"printf 'HELLO-SECTOR-100' | dd of=/dev/vda bs=512 seek=100 conv=sync,fsync "
                  b"2>/dev/null; echo S$((1))W", b"S1W")
        self.poweroff(guest)

        with open(disk, "rb") as f:
            data = f.read()
        self.assertEqual(hashlib.md5(data[3 * MiB:11 * MiB]).hexdigest().encode(), sums[0])
        self.assertEqual(data[100 * SECTOR:101 * SECTOR],
                         b"HELLO-SECTOR-100".ljust(SECTOR, b"\0"))
        self.assertEqual(data[99 * SECTOR:100 * SECTOR], pattern(99))
        self.assertEqual(data[101 * SECTOR:102 * SECTOR], pattern(101))

    # 4、6、7：ext4 挂载、写文件、跨重启持久化。
    def test_ext4_file_persists_across_boots(self):
        disk = self.ext4_image()
        guest = self.boot(disk)
        self.assertIn(b"virtio-blk: /dev/vda mounted on /mnt", guest.output)
        mounts = guest.run(b"grep vda /proc/mounts; echo MO$((1))UNT", b"MO1UNT")
        self.assertIn(b"/dev/vda /mnt ext4 rw", mounts)
        guest.run(b"echo persisted-$((40+2)) > /mnt/hello.txt; "
                  b"mkdir /mnt/dir && seq 1 5000 > /mnt/dir/seq; sync; echo SY$((1))NC", b"SY1NC")
        guest.run(b"umount /mnt; echo UM$((1))OUNT", b"UM1OUNT")
        stats = self.blk_stats(self.poweroff(guest))
        self.assertGreater(int(stats["flushes"]), 0)
        self.assertGreater(int(stats["writes"]), 0)
        self.assertEqual(stats["errors"], "0")

        fsck = subprocess.run(["e2fsck", "-fn", disk], capture_output=True)
        self.assertEqual(fsck.returncode, 0, fsck.stdout + fsck.stderr)
        cat = subprocess.run(["debugfs", "-R", "cat /hello.txt", disk], capture_output=True)
        self.assertEqual(cat.stdout, b"persisted-42\n")

        # 第二次启动：客户机读到上次写的内容。
        guest = self.boot(disk)
        out = guest.run(b"cat /mnt/hello.txt; md5sum < /mnt/dir/seq; echo RE$((2))AD", b"RE2AD")
        self.assertIn(b"persisted-42", out)
        expected = hashlib.md5("".join("%d\n" % i for i in range(1, 5001)).encode()).hexdigest()
        self.assertIn(expected.encode(), out)
        guest.send(b"exit\n")
        guest.expect(b"mini sandbox shell exited")
        self.assertEqual(guest.wait(), 0)
        # /init 在 shell 退出时卸载 /mnt，文件系统是干净的。
        fsck = subprocess.run(["e2fsck", "-fn", disk], capture_output=True)
        self.assertEqual(fsck.returncode, 0, fsck.stdout + fsck.stderr)

    # 8
    def test_bad_disk_is_rejected_before_boot(self):
        tiny = os.path.join(self.tmp, "tiny.img")
        with open(tiny, "wb") as f:
            f.write(b"x" * 100)
        for disk in ("/nonexistent/disk.img", tiny, self.tmp):
            guest = Guest(args=["--initrd", INITRAMFS, "--disk", disk])
            self.addCleanup(guest.close)
            self.assertNotEqual(guest.wait(), 0, disk)
            self.assertIn(b"stage=failed_disk_open", guest.stderr, disk)
            self.assertNotIn(b"Linux version", guest.output, disk)

    # 8：同一个镜像不能同时给两个 VM 用。
    def test_disk_in_use_is_rejected(self):
        disk = self.raw_image(2048)
        first = self.boot(disk)
        second = Guest(args=["--initrd", INITRAMFS, "--disk", disk])
        self.addCleanup(second.close)
        self.assertNotEqual(second.wait(), 0)
        self.assertIn(b"stage=failed_disk_open", second.stderr)
        self.assertIn(b"in use", second.stderr)
        first.run(b"echo ST$((1))ILL", b"ST1ILL")
        self.poweroff(first)

    # 9
    def test_no_disk_keeps_previous_behavior(self):
        guest = boot_to_shell(self, args=["--initrd", INITRAMFS])
        out = guest.run(b"ls /sys/block /sys/bus/platform/devices; echo NO$((1))DISK",
                        b"NO1DISK")
        self.assertNotIn(b"LNRO0005", out)
        self.assertNotIn(b"vda", out)
        self.assertNotIn(b"virtio-blk", guest.output)
        stderr = self.poweroff(guest)
        self.assertNotIn("virtio-blk:", stderr)


if __name__ == "__main__":
    unittest.main()

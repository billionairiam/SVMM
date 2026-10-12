"""Stage 04 端到端测试：svmm 的 CLI 契约，以及运行时配置真的作用到客户机。

先列出这一阶段可能失败的方式，下面每个测试对应其中一条或几条：

  CLI 层（不需要 KVM）
    1. 参数错误时仍然启动了 VM：应在打开 /dev/kvm、加载镜像之前就拒绝；
    2. --mem 接受了非数字、负数、尾随垃圾、溢出或越界的值；
    3. 缺少 --kernel、未知子命令/选项、多余的位置参数被静默忽略；
    4. --help 返回非零或写到 stderr，脚本里没法用；
    5. 内核文件不存在时没有走失败路径（退出码 0 或卡住）。
  运行时（需要 KVM 和内核）
    6. --mem 没有传进客户机：e820 和 MemTotal 仍是默认的 256 MiB；
    7. initrd 地址没有随内存大小变化，或在小内存下与内核解压窗口重叠，
       导致 initramfs 被覆盖、到不了 shell；
    8. --cmdline 没有写进 boot_params，客户机 /proc/cmdline 不一致；
    9. 内存放不下内核窗口 + initramfs 时崩溃、挂起或改坏终端，而不是干净报错。
"""

import os
import re
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_initramfs_shell import (  # noqa: E402
    INITRAMFS, KERNEL, ROOT, VMM, Guest, boot_to_shell, environment_problem,
)

MiB = 1024 * 1024
DEFAULT_CMDLINE = ("console=ttyS0 earlycon=uart8250,io,0x3f8 init=/init loglevel=8 "
                   "acpi_rsdp=0xe0000 reboot=acpi panic=-1")


def setUpModule():
    subprocess.run(["make", "-s", "-C", ROOT, "all"], check=True)


def run_cli(*args):
    return subprocess.run([VMM, *args], cwd=ROOT, stdin=subprocess.DEVNULL,
                          capture_output=True, timeout=30)


def kernel_window_end():
    """按 boot protocol 从 bzImage 启动头算出内核解压窗口的末尾（2 MiB 对齐）。"""
    with open(KERNEL, "rb") as stream:
        header = stream.read(0x264)
    pref_address = struct.unpack_from("<Q", header, 0x258)[0]
    init_size = struct.unpack_from("<I", header, 0x260)[0]
    alignment = struct.unpack_from("<I", header, 0x230)[0]
    relocatable = header[0x234] != 0
    start = max(pref_address, MiB) if relocatable else pref_address
    if relocatable and alignment:
        start = (start + alignment - 1) // alignment * alignment
    end = start + init_size
    return (end + 2 * MiB - 1) // (2 * MiB) * (2 * MiB)


class CliContractTest(unittest.TestCase):
    def assert_rejected(self, *args):
        result = run_cli(*args)
        self.assertEqual(result.returncode, 2, (args, result.stderr))
        self.assertIn(b"ERROR", result.stderr, args)
        # 一旦开始启动，sandbox 会打印 SVMM_METRIC；被拒绝的参数不能走到那一步。
        self.assertNotIn(b"SVMM_METRIC", result.stderr, args)
        self.assertEqual(result.stdout, b"", args)

    def test_help_goes_to_stdout_and_succeeds(self):
        for args in (["--help"], ["-h"], ["run", "--help"], ["run", "--kernel", "x", "-h"]):
            result = run_cli(*args)
            self.assertEqual(result.returncode, 0, args)
            self.assertEqual(result.stderr, b"", args)
            for option in (b"--kernel", b"--initrd", b"--mem", b"--cmdline"):
                self.assertIn(option, result.stdout, args)

    def test_invalid_invocations_are_rejected_before_boot(self):
        cases = [
            [],
            ["start"],
            ["--kernel", "/nonexistent"],
            ["run"],
            ["run", "--mem", "128"],
            ["run", "--kernel", ""],
            ["run", "--kernel", "/nonexistent", "--bogus"],
            ["run", "--kernel", "/nonexistent", "extra"],
            ["run", "--kernel"],
            ["run", "--kernel", "/nonexistent", "--mem"],
        ]
        for args in cases:
            with self.subTest(args=args):
                self.assert_rejected(*args)

    def test_bad_memory_sizes_are_rejected(self):
        for value in ("abc", "128x", "", " 128", "-128", "0", "63", "3073",
                      "18446744073709551617", "0x100"):
            with self.subTest(mem=value):
                self.assert_rejected("run", "--kernel", "/nonexistent", "--mem", value)

    def test_unreadable_kernel_fails_at_image_load(self):
        result = run_cli("run", "--kernel", "/nonexistent/bzImage", "--mem", "64")
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"stage=failed_image_load", result.stderr)
        self.assertEqual(result.stdout, b"")


@unittest.skipIf(environment_problem(), environment_problem() or "")
class RuntimeConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", ROOT, "initramfs"], check=True)

    def boot(self, *args):
        return boot_to_shell(self, args=["--initrd", INITRAMFS, *args])

    def assert_ram_ends_at(self, guest, last_byte):
        # 内核打印 e820 时会合并相邻的 usable 条目，所以只核对最后一个 usable 区间的末尾。
        ends = re.findall(rb"BIOS-e820: \[mem 0x[0-9a-f]+-(0x[0-9a-f]+)\] usable", guest.output)
        self.assertTrue(ends, "kernel did not print the e820 map")
        self.assertEqual(int(ends[-1], 16), last_byte)

    def ramdisk_start(self, guest):
        match = re.search(rb"RAMDISK: \[mem (0x[0-9a-f]+)-", guest.output)
        self.assertIsNotNone(match, "kernel did not report the initramfs location")
        return int(match.group(1), 16)

    def mem_total_kib(self, guest):
        out = guest.run(b"grep MemTotal /proc/meminfo; echo MEM$((7))END", b"MEM7END")
        return int(re.search(rb"MemTotal:\s+(\d+) kB", out).group(1))

    def shutdown(self, guest):
        guest.send(b"poweroff -f\n")
        self.assertEqual(guest.wait(), 0)
        self.assertIn(b"sandbox completed", guest.stderr)

    def test_large_memory_and_custom_cmdline_reach_guest(self):
        cmdline = DEFAULT_CMDLINE + " svmm_marker=stage04"
        guest = self.boot("--mem", "512", "--cmdline", cmdline)
        self.assert_ram_ends_at(guest, 512 * MiB - 1)
        # 512 MiB 时 3/8 规则给出 192 MiB，远高于任何常见内核的解压窗口。
        self.assertEqual(self.ramdisk_start(guest), 192 * MiB)
        self.assertGreater(self.mem_total_kib(guest), 400 * 1024)
        out = guest.run(b"cat /proc/cmdline; echo CMD$((8))END", b"CMD8END")
        self.assertIn(b"svmm_marker=stage04", out)
        self.shutdown(guest)

    def test_small_memory_keeps_initrd_clear_of_kernel_window(self):
        guest = self.boot("--mem", "128")
        self.assert_ram_ends_at(guest, 128 * MiB - 1)
        start = self.ramdisk_start(guest)
        self.assertGreaterEqual(start, max(128 * MiB * 3 // 8, kernel_window_end()))
        total = self.mem_total_kib(guest)
        self.assertLess(total, 128 * 1024)
        self.assertGreater(total, 64 * 1024)
        self.shutdown(guest)

    def test_memory_too_small_for_kernel_and_initrd_fails_cleanly(self):
        initrd_size = os.path.getsize(INITRAMFS)
        start = max(64 * MiB * 3 // 8, kernel_window_end())
        if start + initrd_size <= 64 * MiB - 16 * MiB:
            self.skipTest("this kernel fits in 64 MiB")
        guest = Guest(args=["--initrd", INITRAMFS, "--mem", "64"])
        self.addCleanup(guest.close)
        self.assertEqual(guest.wait(), 1)
        self.assertIn(b"stage=failed_initramfs_load", guest.stderr)
        self.assertIn(b"64 MiB", guest.stderr)
        # 客户机从未运行：串口没有任何输出。
        self.assertEqual(guest.output, b"")


if __name__ == "__main__":
    unittest.main()

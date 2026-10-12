"""Stage 05 端到端测试：运行 bin/virtq-demo，验证 driver/device 握手真的把数据送到了对方。

virtq-demo 在一块 64 KiB 的模拟客户机内存上运行两个场景：
  场景 1  virtio-blk 风格的读：header(RO) + 512 字节数据 buffer(WO)，读第 3 扇区；
  场景 2  scatter/gather 写：header(RO) + 3 段不连续的 64 字节数据(RO)，写第 5 扇区。
模拟磁盘的第 n 扇区初始内容是 "<disk sector n>" 循环填充。

可能失败的方式，以及下面哪个检查防住它：
  1. 演示依赖 KVM/root、崩溃或卡住                      → 带超时运行，退出码 0，stderr 为空
  2. driver 发布了但 device 看不到（idx 没写或写错位置） → 提交后 avail.idx=1、device 视角 count=1
  3. device 读错请求头或走错链，读了别的扇区            → driver 拿到的数据是第 3 扇区的内容
  4. used.len 把只读字节也算进去                        → 读请求 len=512（不是 528），写请求 len=0
  5. scatter/gather 段顺序错、漏段或写越界              → 第 5 扇区 = A*64 + B*64 + C*64，其后不变
  6. device 完成了但 driver 没回收，或计数没归零        → 最终 avail/used 两侧 count 都是 0
  7. 中断丢失或多发                                      → 每个场景恰好一次中断
"""

import os
import re
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMO = os.path.join(ROOT, "bin", "virtq-demo")


def sector_pattern(sector, size=512):
    unit = f"<disk sector {sector}>"
    return (unit * (size // len(unit) + 1))[:size]


def section(text, start, end=None):
    begin = text.index(start)
    finish = text.index(end, begin) if end else len(text)
    return text[begin:finish]


def queue_state(text):
    """从一段 virtq dump 里取出 avail/used 的 idx 和两侧视角的 count。"""
    avail = re.search(r"avail\.idx\s*=\s*(\d+)\n"
                      r"avail\.ring\[\] \(device view: last_seen=(\d+), count=(\d+)\)", text)
    used = re.search(r"used\.idx\s*=\s*(\d+)\n"
                     r"used\.ring\[\] \(driver view: last_used=(\d+), count=(\d+)\)", text)
    assert avail and used, text
    return {
        "avail_idx": int(avail[1]), "last_seen": int(avail[2]), "avail_count": int(avail[3]),
        "used_idx": int(used[1]), "last_used": int(used[2]), "used_count": int(used[3]),
    }


class VirtqDemoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", ROOT, "bin/virtq-demo"], check=True)
        # 去掉 PATH 之外的环境，确认它不需要 /dev/kvm 之类的东西也不读任何输入。
        result = subprocess.run([DEMO], cwd=ROOT, stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=30, env={"PATH": "/usr/bin:/bin"})
        cls.result = result
        cls.out = result.stdout.decode()
        cls.s1 = section(cls.out, "=== Scenario 1", "=== Scenario 2")
        cls.s2 = section(cls.out, "=== Scenario 2")

    def test_runs_cleanly_without_kvm(self):
        self.assertEqual(self.result.returncode, 0, self.result.stderr)
        self.assertEqual(self.result.stderr, b"")

    def test_queue_state_through_handshake(self):
        for scenario, round_no in ((self.s1, 1), (self.s2, 2)):
            submitted = queue_state(section(scenario, "after driver submit", "after device completes"))
            completed = queue_state(section(scenario, "after device completes", "after driver reclaims"))
            reclaimed = queue_state(section(scenario, "after driver reclaims"))
            n = round_no
            self.assertEqual(submitted, {"avail_idx": n, "last_seen": n - 1, "avail_count": 1,
                                         "used_idx": n - 1, "last_used": n - 1, "used_count": 0})
            self.assertEqual(completed, {"avail_idx": n, "last_seen": n, "avail_count": 0,
                                         "used_idx": n, "last_used": n - 1, "used_count": 1})
            self.assertEqual(reclaimed, {"avail_idx": n, "last_seen": n, "avail_count": 0,
                                         "used_idx": n, "last_used": n, "used_count": 0})

    def test_read_request_returns_requested_sector(self):
        self.assertIn("driver: used elem {id=0, len=512}", self.s1)
        match = re.search(r'driver: data buffer @0x[0-9a-f]+ = "([^"]*)"', self.s1)
        self.assertIsNotNone(match, self.s1)
        self.assertEqual(match[1], sector_pattern(3, 64))

    def test_read_chain_is_header_then_writable_buffer(self):
        self.assertRegex(self.s1, r"chain head 0: 2 descriptors, 16 bytes device-readable, "
                                  r"512 bytes device-writable")

    def test_scatter_gather_write_lands_in_order(self):
        self.assertRegex(self.s2, r"chain head 0: 4 descriptors, 208 bytes device-readable, "
                                  r"0 bytes device-writable")
        self.assertIn("driver: used elem {id=0, len=0}", self.s2)
        match = re.search(r'disk sector 5 \[0\.\.256\) = "([^"]*)"', self.s2)
        self.assertIsNotNone(match, self.s2)
        self.assertEqual(match[1], "A" * 64 + "B" * 64 + "C" * 64 + sector_pattern(5)[192:256])

    def test_segments_are_not_contiguous_in_guest_memory(self):
        # 场景 2 的意义在于 device 从不连续的 buffer 收集数据；如果三段恰好相邻，
        # 就退化成了一次连续读。
        rows = re.findall(r"^(\d+)\s+0x([0-9a-f]{16})\s+(\d+)\s+(\S+)\s+(\d+)$",
                          section(self.s2, "after driver submit", "after device completes"), re.M)
        segments = [(int(addr, 16), int(length)) for idx, addr, length, _, _ in rows
                    if idx in ("1", "2", "3")]
        self.assertEqual([length for _, length in segments], [64, 64, 64])
        for (a, la), (b, _) in zip(segments, segments[1:]):
            self.assertNotEqual(a + la, b)

    def test_one_interrupt_per_scenario(self):
        self.assertEqual(len(re.findall(r"^IRQ: ", self.s1, re.M)), 1)
        self.assertEqual(len(re.findall(r"^IRQ: ", self.s2, re.M)), 1)


if __name__ == "__main__":
    unittest.main()

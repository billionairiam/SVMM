"""Stage 10 端到端测试：冷快照（svmm snapshot save/list/validate/delete，run --snapshot）。

先列出可能失败的方式，下面每个测试对应其中一条或几条：

  保存
    1. 快照里的磁盘和源镜像不一致（漏字节、错位、截断）；或把稀疏镜像写成满盘。
    2. 同名快照被覆盖；保存中途失败（磁盘满）留下半个快照或临时目录，
       之后 list/restore 把它当成完整快照。
    3. 正在运行的 VM 的磁盘被复制：拿到的是不一致的文件系统。
  校验（fail-closed）
    4. 坏 manifest 被接受：不是 JSON、截断、尾随垃圾、重复键、版本/类型/名字/
       disk_filename 不对、缺 created_at、大小为负/溢出/与文件不符、
       disk.img 丢失、同样大小但内容被改了。
  恢复
    5. 校验失败仍然改了 --disk，或者仍然创建了 VM（应在打开 /dev/kvm 之前失败）。
    6. 恢复发生在磁盘被打开之后，客户机看到的还是旧内容；或者根本没覆盖。
    7. 覆盖了一个正在运行的 VM 的磁盘。
    8. --disk 是符号链接时把链接本身替换掉；--disk 不存在时没法恢复。
  CLI / 名字
    9. 名字里的 '/'、'..'、空串、超长被接受，读写或删除了快照根目录之外的东西。
   10. --snapshot 不带 --disk、未知 action、缺参数没有报用法错误（退出码 2）。
   11. delete 删了不认识的文件；删不存在的快照返回 0；list 在根目录不存在时出错。
"""

import fcntl
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_initramfs_shell  # noqa: E402
from test_initramfs_shell import INITRAMFS, KERNEL, ROOT, VMM, boot_to_shell  # noqa: E402

MiB = 1024 * 1024


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


class SnapshotTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="svmm-snap-")
        self.addCleanup(shutil.rmtree, self.tmp)
        self.root = os.path.join(self.tmp, "snapshots")
        old = os.environ.get("SVMM_SNAPSHOT_DIR")
        os.environ["SVMM_SNAPSHOT_DIR"] = self.root
        self.addCleanup(lambda: os.environ.__setitem__("SVMM_SNAPSHOT_DIR", old)
                        if old is not None else os.environ.pop("SVMM_SNAPSHOT_DIR"))

    def svmm(self, *args):
        return subprocess.run([VMM, *args], stdin=subprocess.DEVNULL, capture_output=True,
                              timeout=60)

    def ok(self, *args):
        r = self.svmm(*args)
        self.assertEqual(r.returncode, 0, r.stderr.decode(errors="replace"))
        return r

    def data_disk(self, name="disk.img", size=8 * MiB, seed=1):
        """前 1 MiB、中间 1 MiB 是随机数据，其余是洞，末尾 3 字节不对齐。"""
        path = os.path.join(self.tmp, name)
        rnd = random.Random(seed)
        with open(path, "wb") as f:
            f.write(rnd.randbytes(MiB))
            f.seek(size // 2)
            f.write(rnd.randbytes(MiB))
            f.truncate(size + 3)
        return path

    def read(self, path):
        with open(path, "rb") as f:
            return f.read()

    def snap(self, name, *parts):
        return os.path.join(self.root, name, *parts)

    def manifest(self, name):
        with open(self.snap(name, "metadata.json")) as f:
            return json.load(f)

    # 1、9（128 字节的名字合法）、11
    def test_save_list_validate_delete(self):
        disk = self.data_disk()
        self.assertEqual(self.ok("snapshot", "list").stdout, b"")  # 根目录还不存在

        r = self.ok("snapshot", "save", "--name", "ws-1", "--disk", disk,
                    "--kernel", KERNEL, "--mem", "128", "--cmdline", 'a "quoted" \\ arg')
        self.assertIn(b"snapshot 'ws-1' saved successfully", r.stderr)
        self.assertEqual(self.read(self.snap("ws-1", "disk.img")), self.read(disk))
        # 稀疏：随机数据只有 2 MiB，副本不能占满 8 MiB。
        self.assertLess(os.stat(self.snap("ws-1", "disk.img")).st_blocks * 512, 4 * MiB)

        m = self.manifest("ws-1")
        self.assertEqual(m["manifest_version"], 1)
        self.assertEqual(m["snapshot_type"], "cold")
        self.assertEqual(m["name"], "ws-1")
        self.assertEqual(m["disk_filename"], "disk.img")
        self.assertEqual(m["disk_size_bytes"], os.path.getsize(disk))
        self.assertEqual(m["disk_path"], os.path.realpath(disk))
        self.assertEqual(m["guest_mem_mb"], 128)
        self.assertEqual(m["cmdline"], 'a "quoted" \\ arg')
        self.assertRegex(m["created_at"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
        self.assertRegex(m["disk_fnv1a64"], r"^[0-9a-f]{16}$")

        long_name = "x" * 128
        self.ok("snapshot", "save", "--name", long_name, "--disk", disk)
        self.assertIn(b"manifest valid", self.ok("snapshot", "validate", "--name", "ws-1").stderr)

        lines = self.ok("snapshot", "list").stdout.decode().splitlines()
        self.assertEqual([line.split()[0] for line in lines], ["ws-1", long_name])
        self.assertEqual(lines[0].split()[1], str(os.path.getsize(disk)))
        self.assertEqual(lines[0].split()[2], m["created_at"])

        # 同名不覆盖，原快照保持不变。
        other = self.data_disk("other.img", seed=2)
        r = self.svmm("snapshot", "save", "--name", "ws-1", "--disk", other)
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"already exists", r.stderr)
        self.assertEqual(self.read(self.snap("ws-1", "disk.img")), self.read(disk))

        r = self.ok("snapshot", "delete", "--name", "ws-1")
        self.assertIn(b"snapshot 'ws-1' deleted", r.stderr)
        self.assertFalse(os.path.exists(self.snap("ws-1")))
        self.assertEqual(self.svmm("snapshot", "delete", "--name", "ws-1").returncode, 1)

        # 目录里有不认识的文件：拒绝删除，什么都不动。
        open(self.snap(long_name, "notes.txt"), "w").close()
        self.assertEqual(self.svmm("snapshot", "delete", "--name", long_name).returncode, 1)
        self.assertTrue(os.path.exists(self.snap(long_name, "disk.img")))

    # 4、5
    def test_corrupt_snapshots_are_rejected(self):
        disk = self.data_disk()
        self.ok("snapshot", "save", "--name", "good", "--disk", disk)
        target = self.data_disk("target.img", seed=3)
        target_bytes = self.read(target)
        good_meta = self.read(self.snap("good", "metadata.json"))
        good_disk = self.read(disk)
        m = self.manifest("good")

        def with_fields(**changes):
            d = dict(m)
            for k, v in changes.items():
                if v is None:
                    d.pop(k)
                else:
                    d[k] = v
            return json.dumps(d).encode()

        meta_cases = {
            "not json": b"hello",
            "empty": b"",
            "truncated": good_meta[:len(good_meta) // 2],
            "trailing garbage": good_meta + b"x",
            "two objects": good_meta + good_meta,
            "duplicate key": good_meta.replace(b"{", b'{"name": "good",', 1),
            "array": b"[" + good_meta + b"]",
            "version 2": with_fields(manifest_version=2),
            "version string": with_fields(manifest_version="1"),
            "version missing": with_fields(manifest_version=None),
            "type warm": with_fields(snapshot_type="warm"),
            "name mismatch": with_fields(name="other"),
            "disk_filename": with_fields(disk_filename="../target.img"),
            "created_at missing": with_fields(created_at=None),
            "created_at empty": with_fields(created_at=""),
            "size mismatch": with_fields(disk_size_bytes=m["disk_size_bytes"] - 1),
            "size zero": with_fields(disk_size_bytes=0),
            "size negative": with_fields(disk_size_bytes=-1),
            "size overflow": with_fields(disk_size_bytes=10 ** 30),
            "size float": with_fields(disk_size_bytes=float(m["disk_size_bytes"])),
            "hash mismatch": with_fields(disk_fnv1a64="0" * 16),
            "hash missing": with_fields(disk_fnv1a64=None),
        }
        flipped = bytearray(good_disk)
        flipped[MiB // 2] ^= 1
        disk_cases = {"disk content changed": bytes(flipped), "disk missing": None,
                      "disk truncated": good_disk[:-1]}

        def check_rejected(case):
            r = self.svmm("snapshot", "validate", "--name", "good")
            self.assertEqual(r.returncode, 1, case)
            self.assertNotIn(b"manifest valid", r.stderr, case)
            r = self.svmm("run", "--kernel", KERNEL, "--disk", target, "--snapshot", "good")
            self.assertEqual(r.returncode, 1, case)
            self.assertIn(b"stage=failed_snapshot", r.stderr, case)
            self.assertNotIn(b"image_loaded", r.stderr, case)
            self.assertEqual(self.read(target), target_bytes, case)
            self.assertEqual(sorted(os.listdir(self.tmp)),
                             ["disk.img", "snapshots", "target.img"], case)

        for case, content in meta_cases.items():
            with open(self.snap("good", "metadata.json"), "wb") as f:
                f.write(content)
            check_rejected(case)
        with open(self.snap("good", "metadata.json"), "wb") as f:
            f.write(good_meta)
        for case, content in disk_cases.items():
            path = self.snap("good", "disk.img")
            if content is None:
                os.unlink(path)
            else:
                with open(path, "wb") as f:
                    f.write(content)
            check_rejected(case)
        with open(self.snap("good", "disk.img"), "wb") as f:
            f.write(good_disk)
        self.ok("snapshot", "validate", "--name", "good")
        # 坏快照在 list 里标成 invalid，而不是消失或让 list 失败。
        os.unlink(self.snap("good", "metadata.json"))
        self.assertIn(b"invalid", self.ok("snapshot", "list").stdout)

    # 8
    def test_restore_creates_disk_and_follows_symlink(self):
        disk = self.data_disk()
        self.ok("snapshot", "save", "--name", "s", "--disk", disk)
        fresh = os.path.join(self.tmp, "new", "fresh.img")
        os.mkdir(os.path.dirname(fresh))
        r = self.svmm("run", "--kernel", "/nonexistent", "--disk", fresh, "--snapshot", "s")
        self.assertIn(b"workspace restored from snapshot 's'", r.stderr)
        self.assertEqual(self.read(fresh), self.read(disk))
        self.assertEqual(os.listdir(os.path.dirname(fresh)), ["fresh.img"])

        real = self.data_disk("real.img", seed=4)
        os.chmod(real, 0o640)
        link = os.path.join(self.tmp, "link.img")
        os.symlink(real, link)
        self.svmm("run", "--kernel", "/nonexistent", "--disk", link, "--snapshot", "s")
        self.assertTrue(os.path.islink(link))
        self.assertEqual(self.read(real), self.read(disk))
        self.assertEqual(os.stat(real).st_mode & 0o777, 0o640)

    # 3、7：flock 和 virtio_blk_open 用的是同一把锁。
    def test_disk_in_use_is_refused(self):
        disk = self.data_disk()
        self.ok("snapshot", "save", "--name", "s", "--disk", disk)
        other = self.data_disk("other.img", seed=5)
        before = self.read(other)
        with open(other, "rb") as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            r = self.svmm("snapshot", "save", "--name", "busy", "--disk", other)
            self.assertEqual(r.returncode, 1)
            self.assertIn(b"in use", r.stderr)
            self.assertFalse(os.path.exists(self.snap("busy")))
            r = self.svmm("run", "--kernel", KERNEL, "--disk", other, "--snapshot", "s")
            self.assertEqual(r.returncode, 1)
            self.assertIn(b"in use", r.stderr)
            self.assertEqual(self.read(other), before)
        self.assertEqual(sorted(os.listdir(self.root)), ["s"])

    # 2：磁盘满时不留半个快照。
    @unittest.skipUnless(os.geteuid() == 0, "mounting tmpfs requires root")
    def test_failed_save_leaves_nothing(self):
        disk = self.data_disk()
        os.mkdir(self.root)
        subprocess.run(["mount", "-t", "tmpfs", "-o", "size=1m", "tmpfs", self.root], check=True)
        self.addCleanup(subprocess.run, ["umount", self.root], check=True)
        r = self.svmm("snapshot", "save", "--name", "s", "--disk", disk)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(os.listdir(self.root), [])
        self.assertEqual(self.ok("snapshot", "list").stdout, b"")

    # 9
    def test_bad_names_never_escape_root(self):
        disk = self.data_disk()
        self.ok("snapshot", "save", "--name", "keep", "--disk", disk)
        outside = os.path.join(self.tmp, "outside")
        os.mkdir(outside)
        for f in ("disk.img", "metadata.json"):
            shutil.copy(self.snap("keep", f), os.path.join(outside, f))
        for name in ("", ".", "..", "../outside", "a/b", "/tmp", "x" * 129, "a b", ".tmp-x",
                     "keep\n"):
            for args in (["snapshot", "save", "--name", name, "--disk", disk],
                         ["snapshot", "validate", "--name", name],
                         ["snapshot", "delete", "--name", name],
                         ["run", "--kernel", KERNEL, "--disk", disk, "--snapshot", name]):
                r = self.svmm(*args)
                self.assertIn(r.returncode, (1, 2), (name, args))
                self.assertIn(b"invalid snapshot name", r.stderr, (name, args))
        self.assertEqual(sorted(os.listdir(outside)), ["disk.img", "metadata.json"])
        self.assertEqual(sorted(os.listdir(self.root)), ["keep"])

    # 10
    def test_cli_usage_errors(self):
        r = self.ok("--help")
        self.assertIn(b"snapshot", r.stdout)
        self.assertIn(b"--snapshot", r.stdout)
        r = self.ok("snapshot", "--help")
        self.assertIn(b"validate", r.stdout)
        cases = [
            ["snapshot"],
            ["snapshot", "frobnicate"],
            ["snapshot", "save", "--name", "x"],
            ["snapshot", "save", "--disk", "x"],
            ["snapshot", "validate"],
            ["snapshot", "list", "extra"],
            ["snapshot", "save", "--name", "x", "--disk", "d", "--mem", "abc"],
            ["run", "--kernel", KERNEL, "--snapshot", "x"],
            ["run", "--kernel", KERNEL, "--disk", "d", "--snapshot"],
        ]
        for args in cases:
            r = self.svmm(*args)
            self.assertEqual(r.returncode, 2, (args, r.stderr))
            self.assertIn(b"ERROR:", r.stderr, args)
        self.assertFalse(os.path.exists(self.root))


class ColdSnapshotBootTest(unittest.TestCase):
    """6、3、7：真实客户机写文件 → 关机 → save → 改坏 → run --snapshot → 文件回来。"""

    def setUp(self):
        problem = kvm_problem()
        if problem:
            if os.environ.get("BZIMAGE_PATH"):
                raise RuntimeError(problem)
            self.skipTest(problem)
        test_initramfs_shell.KERNEL = KERNEL
        self.tmp = tempfile.mkdtemp(prefix="svmm-snap-")
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

    def boot(self, *extra):
        return boot_to_shell(self, args=["--initrd", INITRAMFS, "--disk", self.disk, *extra])

    def shutdown(self, guest):
        """shell exit：/init 先 sync + umount /mnt 再 reboot，镜像是干净的。"""
        guest.send(b"exit\n")
        self.assertEqual(guest.wait(), 0)

    def test_workspace_survives_restore(self):
        guest = self.boot()
        guest.run(b"echo v1 > /mnt/marker; mkdir /mnt/proj; echo code > /mnt/proj/main.c; "
                  b"sync; echo W$((1))OK", b"W1OK")
        # 7：VM 运行中不能保存，也不能把快照恢复到它的磁盘上。
        r = self.svmm("snapshot", "save", "--name", "live", "--disk", self.disk)
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"in use", r.stderr)
        self.shutdown(guest)

        r = self.svmm("snapshot", "save", "--name", "ws", "--disk", self.disk,
                      "--kernel", KERNEL, "--initrd", INITRAMFS)
        self.assertEqual(r.returncode, 0, r.stderr)

        guest = self.boot()
        guest.run(b"echo v2 > /mnt/marker; rm -r /mnt/proj; echo x > /mnt/junk; "
                  b"echo W$((2))OK", b"W2OK")
        self.shutdown(guest)

        guest = self.boot("--snapshot", "ws")
        out = guest.run(b"cat /mnt/marker /mnt/proj/main.c; ls /mnt; echo R$((3))OK", b"R3OK")
        self.assertRegex(out, rb"v1\r?\ncode\r?\n")
        self.assertNotIn(b"junk", out)
        r = self.svmm("run", "--kernel", KERNEL, "--disk", self.disk, "--snapshot", "ws")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"in use", r.stderr)
        guest.run(b"echo A$((4))LIVE", b"A4LIVE")
        self.shutdown(guest)
        err = guest.stderr.decode(errors="replace")
        self.assertLess(err.index("restoring workspace from cold snapshot 'ws'"),
                        err.index("workspace restored from snapshot 'ws'"))
        self.assertLess(err.index("workspace restored"), err.index("stage=image_loaded"))
        fsck = subprocess.run(["e2fsck", "-fn", self.disk], capture_output=True)
        self.assertEqual(fsck.returncode, 0, fsck.stdout)


if __name__ == "__main__":
    unittest.main()

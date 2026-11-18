"""Hardware E2E for TD launch, isolated I/O, and OCI workload completion."""
import os
from pathlib import Path
import shutil
import subprocess
import struct
import tempfile
import unittest

from tests import test_run_image

ROOT = Path(__file__).resolve().parent.parent
VMM = ROOT / "bin/svmm"
KERNEL = os.environ.get("SVMM_TEST_TDX_KERNEL")
SHIM = os.environ.get("SVMM_TEST_TD_SHIM")
IMAGE = os.environ.get("SVMM_TEST_TDX_IMAGE", "ubuntu:24.04")


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], **kwargs)


class TdxTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not KERNEL or not SHIM:
            raise unittest.SkipTest("set SVMM_TEST_TDX_KERNEL and SVMM_TEST_TD_SHIM")
        run("make", "-s", "all", "workload-initramfs", cwd=ROOT, check=True)

    def setUp(self):
        self.tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def boot(self, *args, timeout=40, **kwargs):
        return run(VMM, "run", "--tdx", "--td-shim", SHIM, "--kernel", KERNEL,
                   *args, timeout=timeout, stdin=subprocess.DEVNULL, capture_output=True, **kwargs)

    def assert_status(self, result, expected):
        self.assertEqual(result.returncode, expected,
                         (result.stdout + result.stderr)[-9000:].decode(errors="replace"))

    def test_linux_reports_non_debug_tdx(self):
        init = self.tmp / "init"
        archive = self.tmp / "boot.cpio.gz"
        run(os.environ.get("GUEST_CC", "musl-gcc"), "-static", "-O2",
            ROOT / "tests/fixtures/tdx-report-init.c", "-o", init, check=True)
        run("sh", ROOT / "rootfs/build-workload-initramfs.sh", init, archive, check=True)
        result = self.boot("--initrd", archive)
        self.assert_status(result, 0)
        self.assertIn(b"SVMM_TDREPORT_OK", result.stdout, result.stdout[-4000:])

    def test_invalid_firmware_and_incompatible_flags(self):
        broken = self.tmp / "bad.bin"
        broken.write_bytes(b"bad firmware")
        result = self.boot("--td-shim", broken)
        self.assert_status(result, 1)
        self.assertIn(b"TD-Shim", result.stderr)
        self.assertNotIn(b"stage=kvm_run", result.stderr)
        for options in (("--tap", "tap0"), ("--jail",), ("--warm-save", "app")):
            self.assert_status(self.boot(*options), 2)

    def test_kernel_window_cannot_overlap_shim_runtime(self):
        # Place the decompressor's end at the advertised top of RAM, inside
        # the shim's live page tables and other reserved runtime data.
        kernel = self.tmp / "overlap-bzImage"
        data = bytearray(Path(KERNEL).read_bytes())
        preferred, = struct.unpack_from("<Q", data, 0x258)
        alignment, relocatable = struct.unpack_from("<IB", data, 0x230)
        start = preferred
        if relocatable:
            start = (max(preferred, 1 << 20) + alignment - 1) & -alignment
        memory_mb = max(64, (start + len(data) + (1 << 20) - 1) >> 20)
        struct.pack_into("<I", data, 0x260, (memory_mb << 20) - start)
        kernel.write_bytes(data)
        result = self.boot("--kernel", kernel, "--mem", memory_mb,
                           "--cmdline", "panic=1", timeout=12)
        self.assert_status(result, 1)
        self.assertIn(b"overlaps the TD-Shim runtime reservation", result.stderr)
        self.assertNotIn(b"TDX launch finalized", result.stderr)

    def test_workload_rw_and_exit(self):
        disk = self.tmp / "workload.ext4"
        if not shutil.which("docker"):
            self.fail("Docker is required for the TDX workload test")
        run("python3", ROOT / "scripts/build-workload.py", "--image",
            IMAGE, "--output", disk,
            "--", "/bin/sh", "-ec", "cat /etc/os-release; dd if=/dev/zero of=/tdx-write bs=4096 count=512; printf tdx-ok > /result; sync; exit 7",
            check=True, timeout=80)
        result = self.boot("--initrd", ROOT / "build/workload-initramfs.cpio.gz",
                           "--disk", disk, "--workload")
        self.assert_status(result, 7)
        run("e2fsck", "-fn", disk, capture_output=True, check=True)
        read = run("debugfs", "-R", "cat /result", disk, capture_output=True, check=True)
        self.assertEqual(read.stdout, b"tdx-ok")

    def test_run_image_cleans_disk_and_preserves_status(self):
        result = self.boot("--image", IMAGE,
                           "--", "/bin/sh", "-c", "echo SVMM_TDX_IMAGE_OK; exit 7",
                           env={**os.environ, "TMPDIR": str(self.tmp)})
        self.assert_status(result, 7)
        self.assertIn(b"SVMM_TDX_IMAGE_OK", result.stdout)
        self.assertEqual(list(self.tmp.iterdir()), [])

    def test_auto_kernel_selection_rejects_non_tdx_kernel(self):
        # Give the launcher a newer, ordinary KVM kernel candidate in a
        # private mount namespace. The valid older candidate is a real
        # bzImage and must run an application inside the TD.
        boot_dir = self.tmp / "boot"
        scratch = self.tmp / "scratch"
        boot_dir.mkdir()
        scratch.mkdir()
        shutil.copyfile(KERNEL, boot_dir / "vmlinuz-tdx")
        (boot_dir / "vmlinuz-ordinary").write_bytes(b"ordinary kernel candidate")
        base = [
            "ACPI", "BINFMT_ELF", "DEVTMPFS", "VIRTIO_MMIO", "VIRTIO_BLK",
            "EXT4_FS", "SERIAL_8250_CONSOLE", "X86_IOPL_IOPERM", "NET", "INET",
            "VIRTIO_NET", "BLK_DEV_INITRD", "RD_GZIP", "PROC_FS", "SYSFS", "TMPFS",
            "UNIX98_PTYS",
        ]
        for variant, extra in (("ordinary", []), ("tdx", [
                "INTEL_TDX_GUEST", "KVM_GUEST", "X86_X2APIC", "SWIOTLB", "DEVPORT"])):
            (boot_dir / f"config-{variant}").write_text(
                "".join(f"CONFIG_{name}=y\n" for name in base + extra))
        os.utime(boot_dir / "vmlinuz-tdx", (1, 1))
        os.utime(boot_dir / "vmlinuz-ordinary", (2, 2))
        env = {**os.environ, "SVMM_TD_SHIM": SHIM, "TMPDIR": str(scratch)}
        env.pop("SVMM_KERNEL", None)
        result = run(
            "unshare", "--mount", "--propagation", "private", "sh", "-c",
            'mount --bind "$1" /boot || exit; shift; exec "$@"',
            "svmm-tdx-kernel-fixture", boot_dir, VMM, "run", "--tdx", "--image",
            IMAGE,
            "--", "/bin/sh", "-c", "echo SVMM_TDX_AUTO_OK; exit 7",
            timeout=40, stdin=subprocess.DEVNULL, capture_output=True, env=env, cwd=self.tmp,
        )
        self.assert_status(result, 7)
        self.assertIn(b"SVMM_TDX_AUTO_OK", result.stdout)
        self.assertEqual(list(scratch.iterdir()), [])

    def test_invalid_shim_metadata_fails_before_image_preparation(self):
        data = bytearray(Path(SHIM).read_bytes())
        descriptor, = struct.unpack_from("<I", data, len(data) - 32)
        count, = struct.unpack_from("<I", data, descriptor + 12)
        for index in range(count):
            entry = descriptor + 16 + index * 32
            kind, flags = struct.unpack_from("<II", data, entry + 24)
            if kind == 0 and flags == 1:  # BFV must have EXTENDMR
                struct.pack_into("<I", data, entry + 28, 0)
                break
        else:
            self.fail("test firmware has no measured BFV section")
        firmware = self.tmp / "corrupt-bfv.bin"
        firmware.write_bytes(data)
        result = self.boot("--td-shim", firmware, "--image", "127.0.0.1:1/must-not-pull:latest",
                           env={**os.environ, "TMPDIR": str(self.tmp)})
        self.assert_status(result, 125)
        self.assertIn(b"invalid or unsupported TD-Shim metadata", result.stderr)
        self.assertNotIn(b"svmm: image ", result.stderr)

    def test_invalid_shim_fails_before_image_preparation(self):
        result = self.boot("--td-shim", self.tmp / "missing.bin",
                           "--image", "127.0.0.1:1/must-not-pull:latest",
                           env={**os.environ, "TMPDIR": str(self.tmp)})
        self.assert_status(result, 125)
        self.assertIn(b"TD-Shim", result.stderr)
        self.assertNotIn(b"svmm: image ", result.stderr)
        self.assertEqual(list(self.tmp.iterdir()), [])


class TdxImageDefaultsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not KERNEL or not SHIM:
            raise unittest.SkipTest("set SVMM_TEST_TDX_KERNEL and SVMM_TEST_TD_SHIM")
        run("make", "-s", "all", "workload-initramfs", cwd=ROOT, check=True)
        cls.image = "svmm-tdx-e2e:%d" % os.getpid()
        cls.addClassCleanup(run, "docker", "image", "rm", cls.image, capture_output=True)
        with tempfile.TemporaryDirectory() as context:
            (Path(context) / "Dockerfile").write_text(
                'ARG BASE\nFROM ${BASE}\nENTRYPOINT ["/bin/sh", "-c"]\n'
                'CMD ["echo SVMM_TDX_IMAGE_DEFAULT_OK; exit 7"]\n')
            run("docker", "build", "--network=none", "-q", "-t", cls.image,
                "--build-arg", "BASE=" + IMAGE, context, timeout=90, check=True)

    def setUp(self):
        self.tmp = Path(self.enterContext(tempfile.TemporaryDirectory()))

    assert_finished = test_run_image.RunImageTest.assert_finished

    def run_image(self, *, image=None):
        return run(VMM, "run", "--tdx", "--td-shim", SHIM, "--kernel", KERNEL,
                   "--image", image or self.image, cwd=self.tmp,
                   env={**os.environ, "TMPDIR": str(self.tmp)}, timeout=90,
                   stdin=subprocess.DEVNULL, capture_output=True)

    def test_one_command_runs_image_defaults_from_another_directory(self):
        result = self.run_image()
        self.assert_finished(result)
        self.assertIn(b"SVMM_TDX_IMAGE_DEFAULT_OK", result.stdout)

    test_missing_local_image_is_pulled_from_real_registry_and_run = (
        test_run_image.RunImageTest.test_missing_local_image_is_pulled_from_real_registry_and_run)

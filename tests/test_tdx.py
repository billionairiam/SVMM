"""Hardware E2E for TD launch, isolated I/O, and OCI workload completion."""
import ctypes
import fcntl
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import tempfile
import unittest

from tests import test_run_image
from tests.test_virtio_net import NetNS, net_stats

ROOT = Path(__file__).resolve().parent.parent
VMM = ROOT / "bin/svmm"
KERNEL = os.environ.get("SVMM_TEST_TDX_KERNEL")
SHIM = os.environ.get("SVMM_TEST_TD_SHIM")
IMAGE = os.environ.get("SVMM_TEST_TDX_IMAGE", "ubuntu:24.04")
WORKLOAD_OPTIONS = (
    "ACPI", "BINFMT_ELF", "DEVTMPFS", "VIRTIO_MMIO", "VIRTIO_BLK", "EXT4_FS",
    "SERIAL_8250_CONSOLE", "NET", "INET", "BLK_DEV_INITRD", "RD_GZIP", "PROC_FS",
    "SYSFS", "TMPFS", "UNIX98_PTYS",
)
TDX_OPTIONS = ("INTEL_TDX_GUEST", "KVM_GUEST", "X86_X2APIC", "SWIOTLB", "DEVPORT")


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], **kwargs)


def write_config(path, options):
    path.write_text("".join(f"CONFIG_{name}=y\n" for name in options))


def tdx_supports_avx_state():
    # TDX_CAPABILITIES on the VM fd. The XFAM member is at offset 8;
    # the trailing kvm_cpuid2 array starts at offset 2048 in this ABI.
    cpuid_capacity = 256
    caps = ctypes.create_string_buffer(2048 + 8 + 40 * cpuid_capacity)
    struct.pack_into("<I", caps, 2048, cpuid_capacity)
    with open("/dev/kvm", "rb+") as kvm:
        vm = fcntl.ioctl(kvm, 0xae01, 5)  # KVM_CREATE_VM(KVM_X86_TDX_VM)
        try:
            command = struct.pack("<IIQQ", 0, 0, ctypes.addressof(caps), 0)
            fcntl.ioctl(vm, 0xc008aeba, command)  # KVM_MEMORY_ENCRYPT_OP
        finally:
            os.close(vm)
    xfam, = struct.unpack_from("<Q", caps, 8)
    return bool(xfam & (1 << 2))


def kernel_runtime_address(data):
    preferred, = struct.unpack_from("<Q", data, 0x258)
    alignment, relocatable = struct.unpack_from("<IB", data, 0x230)
    if not relocatable:
        return preferred
    return (max(preferred, 1 << 20) + alignment - 1) & -alignment


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

    def boot_with_candidates(self, boot_dir, runtime, *args, binary=VMM, timeout=80):
        env = {**os.environ, "SVMM_TD_SHIM": SHIM, "TMPDIR": str(runtime)}
        env.pop("SVMM_KERNEL", None)
        env.pop("SVMM_INITRD", None)
        return run(
            "unshare", "--mount", "--propagation", "private", "sh", "-c",
            'mount --bind "$1" /boot || exit; shift; exec "$@"',
            "svmm-tdx-kernel-fixture", boot_dir, binary, "run", "--tdx", *args,
            timeout=timeout, stdin=subprocess.DEVNULL, capture_output=True, env=env, cwd=self.tmp,
        )

    def report_initrd(self):
        init = self.tmp / "init"
        archive = self.tmp / "boot.cpio.gz"
        run(os.environ.get("GUEST_CC", "musl-gcc"), "-static", "-O2",
            ROOT / "tests/fixtures/tdx-report-init.c", "-o", init, check=True)
        run("sh", ROOT / "rootfs/build-workload-initramfs.sh", init, archive, check=True)
        return archive

    def test_linux_reports_non_debug_tdx(self):
        result = self.boot("--initrd", self.report_initrd())
        self.assert_status(result, 0)
        self.assertIn(b"SVMM_TDREPORT_OK", result.stdout, result.stdout[-4000:])

    def test_guest_can_execute_avx(self):
        if not tdx_supports_avx_state():
            self.skipTest("KVM TDX does not expose AVX in supported XFAM")
        result = self.boot("--initrd", self.report_initrd())
        self.assert_status(result, 0)
        self.assertIn(b"SVMM_TD_AVX_OK", result.stdout[-4000:])

    def test_guest_sees_ram_above_four_gib(self):
        result = self.boot("--mem", "4096", "--initrd", self.report_initrd(), timeout=150)
        self.assert_status(result, 0)
        self.assertIn(b"SVMM_TDREPORT_OK", result.stdout, result.stdout[-3000:])
        memory = re.search(rb"SVMM_TD_RAM=(\d+)", result.stdout)
        self.assertIsNotNone(memory, result.stdout[-3000:])
        self.assertGreater(int(memory[1]), 3584 * 1024 * 1024)
        self.assertRegex(result.stdout[:10000],
                         rb"\[mem 0x0000000100000000-0x000000013fffffff\] +(?:usable|System RAM)")

    def test_invalid_firmware_and_incompatible_flags(self):
        broken = self.tmp / "bad.bin"
        broken.write_bytes(b"bad firmware")
        result = self.boot("--td-shim", broken)
        self.assert_status(result, 1)
        self.assertIn(b"TD-Shim", result.stderr)
        self.assertNotIn(b"stage=kvm_run", result.stderr)
        for options in (("--jail",), ("--warm-save", "app")):
            self.assert_status(self.boot(*options), 2)

    def test_kernel_window_cannot_overlap_shim_runtime(self):
        # Place the decompressor's end at the advertised top of RAM, inside
        # the shim's live page tables and other reserved runtime data.
        kernel = self.tmp / "overlap-bzImage"
        data = bytearray(Path(KERNEL).read_bytes())
        start = kernel_runtime_address(data)
        memory_mb = max(64, (start + len(data) + (1 << 20) - 1) >> 20)
        struct.pack_into("<I", data, 0x260, (memory_mb << 20) - start)
        kernel.write_bytes(data)
        result = self.boot("--kernel", kernel, "--mem", memory_mb,
                           "--cmdline", "panic=1", timeout=12)
        self.assert_status(result, 1)
        self.assertIn(b"overlaps the TD-Shim runtime reservation", result.stderr)
        self.assertNotIn(b"TDX launch finalized", result.stderr)

    def test_boots_kernel_with_file_extending_beyond_shim_payload(self):
        firmware = Path(SHIM).read_bytes()
        table, = struct.unpack_from("<I", firmware, len(firmware) - 32)
        count, = struct.unpack_from("<I", firmware, table + 12)
        for i in range(count):
            _, _, start, size, kind, _ = struct.unpack_from("<IIQQII", firmware, table + 16 + 32 * i)
            if kind == 5:  # TD-Shim payload
                break
        else:
            self.fail("test firmware lacks the Linux payload region")
        kernel = self.tmp / "large-bzImage"
        data = Path(KERNEL).read_bytes()
        setup_size = ((data[0x1f1] or 4) + 1) * 512
        payload_size = start + size - kernel_runtime_address(data) + 4096
        init_size, = struct.unpack_from("<I", data, 0x260)
        if payload_size > init_size:
            self.skipTest("test kernel cannot be extended past the firmware payload window")
        # Preserve the executable compressed stream and extend the file's
        # tail so the bytes loaded by the VMM cross the old payload bound.
        kernel.write_bytes(data)
        if len(data) < setup_size + payload_size:
            with kernel.open("r+b") as output:
                output.truncate(setup_size + payload_size)
        result = self.boot("--kernel", kernel, "--initrd", self.report_initrd())
        self.assert_status(result, 0)
        self.assertIn(b"SVMM_TDREPORT_OK", result.stdout[-3000:])

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

    def test_image_exchanges_tcp_data_over_tap_in_netns(self):
        # A packaged offline kernel must not hide a TAP-capable /boot kernel.
        package = self.tmp / "package"
        boot_dir = self.tmp / "boot"
        runtime = self.tmp / "runtime"
        boot_dir.mkdir()
        runtime.mkdir()
        for asset in ("bin/svmm", "scripts/run-image.py", "scripts/build-workload.py",
                      "build/workload-initramfs.cpio.gz"):
            dest = package / asset
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / asset, dest)
        images = package / "images"
        images.mkdir()
        shutil.copyfile(KERNEL, images / "tdx-bzImage")
        write_config(images / "tdx-bzImage.config", WORKLOAD_OPTIONS + TDX_OPTIONS)
        shutil.copyfile(KERNEL, boot_dir / "vmlinuz-tdx")
        write_config(boot_dir / "config-tdx", WORKLOAD_OPTIONS + TDX_OPTIONS + ("VIRTIO_NET",))
        ns = NetNS(self, "tdx")
        ns.sh("ip tuntap add tap0 mode tap && ip addr add 192.168.100.1/24 dev tap0 && "
              "ip link set tap0 up")
        server = ns.popen(["python3", "-u", "-c", """
import hashlib, socket
with socket.create_server(('192.168.100.1', 0)) as listener:
    print(listener.getsockname()[1], flush=True)
    listener.settimeout(60)
    with listener.accept()[0] as client, client.makefile('rb') as data:
        payload = data.read(262144)
        assert len(payload) == 262144 and not any(payload)
        client.sendall(hashlib.sha256(payload).hexdigest().encode() + b'\\n' + b'A' * 262144)
"""], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(server.communicate)
        self.addCleanup(server.kill)
        port = server.stdout.readline().strip().decode()
        self.assertTrue(port.isdecimal(), port)
        result = self.boot_with_candidates(
                           boot_dir, runtime, "--tap", "tap0", "--netns", ns.name,
                           "--image", "ubuntu:24.04",
                           "--entrypoint", "/bin/bash", "--", "-ec",
                           f"exec 3<>/dev/tcp/192.168.100.1/{port}; "
                           "dd if=/dev/zero bs=4096 count=64 status=none >&3; "
                           "read -r digest <&3; echo SVMM_TD_TX=$digest; "
                           "echo SVMM_TD_RX=$(head -c 262144 <&3 | sha256sum); exit 7",
                           binary=package / "bin/svmm")
        self.assert_status(result, 7)
        self.assertIn(b"guest kernel /boot/vmlinuz-tdx", result.stderr)
        _, errors = server.communicate(timeout=5)
        self.assertEqual(server.returncode, 0, errors)
        self.assertIn(b"SVMM_TD_TX=" + hashlib.sha256(bytes(262144)).hexdigest().encode(),
                      result.stdout)
        self.assertIn(b"SVMM_TD_RX=" + hashlib.sha256(b"A" * 262144).hexdigest().encode(),
                      result.stdout)
        stats = net_stats(result.stderr)
        self.assertIsNotNone(stats, result.stderr[-2000:])
        self.assertGreater(stats["tx_packets"], 0)
        self.assertGreater(stats["rx_packets"], 0)
        self.assertEqual(stats["bad_chains"], 0)

    def test_auto_kernel_selection_rejects_non_tdx_kernel(self):
        # Give the launcher a newer, ordinary KVM kernel candidate in a
        # private mount namespace. The valid older candidate is a real
        # bzImage and must run an application inside the TD.
        boot_dir = self.tmp / "boot"
        scratch = self.tmp / "scratch"
        boot_dir.mkdir()
        scratch.mkdir()
        shutil.copyfile(KERNEL, boot_dir / "vmlinuz-tdx")
        shutil.copyfile(KERNEL, boot_dir / "vmlinuz-ordinary")
        with open(KERNEL, "rb") as source:
            (boot_dir / "vmlinuz-invalid").write_bytes(source.read(0x264))
        for variant, extra in (("ordinary", ()), ("tdx", TDX_OPTIONS), ("invalid", TDX_OPTIONS)):
            write_config(boot_dir / f"config-{variant}", WORKLOAD_OPTIONS + extra)
        os.utime(boot_dir / "vmlinuz-tdx", (1, 1))
        os.utime(boot_dir / "vmlinuz-ordinary", (2, 2))
        os.utime(boot_dir / "vmlinuz-invalid", (3, 3))
        result = self.boot_with_candidates(boot_dir, scratch, "--image", IMAGE,
                                           "--", "/bin/sh", "-c", "echo SVMM_TDX_AUTO_OK; exit 7")
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

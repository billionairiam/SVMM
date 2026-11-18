#!/usr/bin/env python3
"""Coordinate image preparation and the native SVMM workload path."""
import argparse
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
REQUIRED_KERNEL_OPTIONS = (
    "CONFIG_ACPI=y", "CONFIG_BINFMT_ELF=y", "CONFIG_DEVTMPFS=y",
    "CONFIG_VIRTIO_MMIO=y", "CONFIG_VIRTIO_BLK=y", "CONFIG_EXT4_FS=y",
    "CONFIG_SERIAL_8250_CONSOLE=y", "CONFIG_X86_IOPL_IOPERM=y",
    "CONFIG_NET=y", "CONFIG_INET=y", "CONFIG_VIRTIO_NET=y",
    "CONFIG_BLK_DEV_INITRD=y", "CONFIG_RD_GZIP=y", "CONFIG_PROC_FS=y",
    "CONFIG_SYSFS=y", "CONFIG_TMPFS=y", "CONFIG_UNIX98_PTYS=y",
)


class Cancelled(Exception):
    pass


class Runner:
    def __init__(self):
        self.process = None
        self.vm = False
        self.cancelled = 0

    def signal(self, number, _frame):
        if number == signal.SIGUSR2:
            if not self.vm:
                return
        else:
            self.cancelled = number
        if self.process is not None:
            try:
                self.process.send_signal(number)
            except ProcessLookupError:
                pass

    def child(self, argv, *, vm=False):
        if self.cancelled:
            raise Cancelled()
        self.vm = vm
        process = subprocess.Popen(argv, stdout=None if vm else sys.stderr)
        self.process = process
        try:
            # A signal can arrive between Popen and assignment of self.process.
            if self.cancelled:
                process.send_signal(self.cancelled)
            code = process.wait()
        finally:
            self.process = None
        if not vm and self.cancelled:
            raise Cancelled()
        return code if code >= 0 else 128 - code


def readable_file(path):
    return path.is_file() and os.access(path, os.R_OK)


def tdx_bzimage(path):
    try:
        with path.open("rb") as kernel:
            header = kernel.read(0x264)
            file_size = os.fstat(kernel.fileno()).st_size
    except OSError:
        return False
    if (len(header) != 0x264 or header[0x1fe:0x200] != b"\x55\xaa" or
            header[0x202:0x206] != b"HdrS" or
            struct.unpack_from("<H", header, 0x206)[0] < 0x20c or
            not (header[0x211] & 1 and header[0x236] & 1)):
        return False
    setup_size = ((header[0x1f1] or 4) + 1) * 512
    payload_size = struct.unpack_from("<I", header, 0x1f4)[0] * 16
    init_size = struct.unpack_from("<I", header, 0x260)[0]
    return (setup_size <= 0x10000 and file_size <= 512 * 1024 * 1024 and
            0 < payload_size <= file_size - setup_size <= init_size)


def kernel_config_has(path, required, vcpus):
    try:
        entries = path.read_text().splitlines()
    except (OSError, UnicodeError):
        return False
    if not set(required).issubset(entries):
        return False
    if vcpus == 1:
        return True
    for entry in entries:
        if entry.startswith("CONFIG_NR_CPUS="):
            limit = entry.partition("=")[2]
            return limit.isdecimal() and int(limit) >= vcpus
    return False


def kernel_path(explicit, *, tdx=False, tap=False, vcpus=1):
    configured = explicit if explicit is not None else os.environ.get("SVMM_KERNEL")
    if configured is not None:
        path = Path(configured).absolute()
        if not readable_file(path):
            raise ValueError("guest kernel is not readable: " + str(path))
        if tdx and not tdx_bzimage(path):
            raise ValueError("TDX guest kernel must be a complete 64-bit Linux bzImage: " + str(path))
        return path
    packaged = ROOT / ("images/tdx-bzImage" if tdx else "images/bzImage")
    required = REQUIRED_KERNEL_OPTIONS
    if tdx:
        required = tuple(option for option in required if option != "CONFIG_X86_IOPL_IOPERM=y")
        if not tap:
            required = tuple(option for option in required if option != "CONFIG_VIRTIO_NET=y")
        required += ("CONFIG_INTEL_TDX_GUEST=y", "CONFIG_KVM_GUEST=y", "CONFIG_X86_X2APIC=y",
                     "CONFIG_SWIOTLB=y", "CONFIG_DEVPORT=y")
        if vcpus > 1:
            required += ("CONFIG_SMP=y",)
    if readable_file(packaged):
        if not tdx:
            return packaged
        config = packaged.with_suffix(".config")
        if tdx_bzimage(packaged) and ((not tap and vcpus == 1 and not config.exists()) or
                                     kernel_config_has(config, required, vcpus)):
            return packaged
    candidates = []
    for kernel in Path("/boot").glob("vmlinuz-*"):
        if not readable_file(kernel) or (tdx and not tdx_bzimage(kernel)):
            continue
        version = kernel.name[len("vmlinuz-"):]
        versions = [version]
        for suffix in (".bak", ".old"):
            if version.endswith(suffix):
                versions.append(version[:-len(suffix)])
        for candidate in versions:
            if kernel_config_has(kernel.parent / ("config-" + candidate), required, vcpus):
                candidates.append(kernel)
                break
    if not candidates:
        raise ValueError("no compatible guest kernel found; set SVMM_KERNEL or use --kernel PATH")
    return max(candidates, key=lambda path: path.stat().st_mtime)


def initrd_path(explicit, runner):
    configured = explicit if explicit is not None else os.environ.get("SVMM_INITRD")
    if configured is not None:
        path = Path(configured).absolute()
    else:
        path = ROOT / "build/workload-initramfs.cpio.gz"
        if not readable_file(path):
            print("svmm: preparing guest runtime", file=sys.stderr)
            if runner.child(["make", "-s", "-C", str(ROOT), "workload-initramfs"]):
                raise ValueError("could not build the guest runtime; run make workload-initramfs")
    if not readable_file(path):
        raise ValueError("workload initramfs is not readable: " + str(path))
    return path


def main():
    # The native CLI validated all options and supplies its own absolute path.
    binary, raw = sys.argv[1], sys.argv[2:]
    command = []
    if "--" in raw:
        split = raw.index("--")
        raw, command = raw[:split], raw[split + 1:]
    parser = argparse.ArgumentParser(usage="svmm run --image IMAGE [options] [-- COMMAND ARGS]")
    parser.add_argument("--image", required=True)
    parser.add_argument("-k", "--kernel")
    parser.add_argument("-i", "--initrd")
    parser.add_argument("--entrypoint")
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--workdir")
    parser.add_argument("--user")
    parser.add_argument("--size-mb", type=int)
    parser.add_argument("--workload", action="store_true")
    parser.add_argument("--tdx", action="store_true")
    parser.add_argument("--vcpus", type=int)
    parser.add_argument("--tap")
    options, vm_options = parser.parse_known_args(raw)
    runner = Runner()
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGUSR2):
        signal.signal(number, runner.signal)
    try:
        if os.geteuid() != 0:
            raise ValueError("run --image as root to preserve image file ownership")
        kernel = kernel_path(options.kernel, tdx=options.tdx, tap=options.tap is not None,
                             vcpus=options.vcpus or 1)
        if options.vcpus is not None:
            vm_options.append("--vcpus=" + str(options.vcpus))
        if options.tap is not None:
            vm_options.append("--tap=" + options.tap)
        if options.tdx:
            vm_options.append("--tdx")
        initrd = initrd_path(options.initrd, runner)
        print("svmm: image %s; guest kernel %s" % (options.image, kernel), file=sys.stderr)
        with tempfile.TemporaryDirectory(prefix="svmm-image-") as runtime:
            disk = str(Path(runtime) / "rootfs.ext4")
            builder = [sys.executable, str(ROOT / "scripts/build-workload.py"),
                       "--image=" + options.image, "--output=" + disk]
            for name in ("entrypoint", "workdir", "user", "size_mb"):
                value = getattr(options, name)
                if value is not None:
                    builder += ["--" + name.replace("_", "-") + "=" + str(value)]
            for value in options.env:
                builder += ["--env=" + value]
            if command:
                builder += ["--", *command]
            if runner.child(builder):
                return 125
            return runner.child([binary, "run", *vm_options,
                                 "--kernel", str(kernel), "--initrd", str(initrd),
                                 "--disk", disk, "--workload"], vm=True)
    except Cancelled:
        return 128 + runner.cancelled
    except (OSError, ValueError) as error:
        print("svmm: " + str(error), file=sys.stderr)
        return 125


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Coordinate image preparation and the native SVMM workload path."""
import argparse
import os
from pathlib import Path
import signal
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


def kernel_path(explicit, *, tdx=False):
    configured = explicit if explicit is not None else os.environ.get("SVMM_KERNEL")
    if configured is not None:
        path = Path(configured).absolute()
        if not readable_file(path):
            raise ValueError("guest kernel is not readable: " + str(path))
        return path
    packaged = ROOT / ("images/tdx-bzImage" if tdx else "images/bzImage")
    required = REQUIRED_KERNEL_OPTIONS
    if tdx:
        required = tuple(option for option in required
                         if option not in ("CONFIG_X86_IOPL_IOPERM=y", "CONFIG_VIRTIO_NET=y"))
        required += ("CONFIG_INTEL_TDX_GUEST=y", "CONFIG_KVM_GUEST=y", "CONFIG_X86_X2APIC=y",
                     "CONFIG_SWIOTLB=y", "CONFIG_DEVPORT=y")
    if readable_file(packaged):
        return packaged
    candidates = []
    for kernel in Path("/boot").glob("vmlinuz-*"):
        if not readable_file(kernel):
            continue
        version = kernel.name[len("vmlinuz-"):]
        versions = [version]
        for suffix in (".bak", ".old"):
            if version.endswith(suffix):
                versions.append(version[:-len(suffix)])
        for candidate in versions:
            try:
                config = (kernel.parent / ("config-" + candidate)).read_text().splitlines()
            except OSError:
                continue
            if all(option in config for option in required):
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
    options, vm_options = parser.parse_known_args(raw)
    runner = Runner()
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGUSR2):
        signal.signal(number, runner.signal)
    try:
        if os.geteuid() != 0:
            raise ValueError("run --image as root to preserve image file ownership")
        kernel = kernel_path(options.kernel, tdx=options.tdx)
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

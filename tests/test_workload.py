"""Real Docker -> ext4 -> KVM workload E2E.

Failures this suite must catch BEFORE implementation:
1. Flattened image boots but argv/env/cwd/USER are lost or shell is required.
2. Application status, signal, missing exec or malformed config is reported as success.
3. File ownership/links or writes are lost; shutdown leaves ext4 unclean.
4. Conversion overwrites an existing output or leaks its temporary container.
5. A plain guest poweroff or invalid control message masquerades as application success.
Network and warm-snapshot tests are added after the basic path works.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import tarfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_initramfs_shell as shell
from test_initramfs_shell import Guest, ROOT, VMM

KERNEL = os.environ.get("VIRTIO_BZIMAGE") or shell.KERNEL
BUILDER = str(Path(ROOT) / "scripts/build-workload.py")
INITRD = str(Path(ROOT) / "build/workload-initramfs.cpio.gz")
SPECIAL = "space 'quote' \"double\"\nsecond line"
ENV_VALUE = "value with 'quotes'\nand newline"


class WorkloadTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        problem = None
        if not os.access("/dev/kvm", os.R_OK | os.W_OK) or not os.access(KERNEL, os.R_OK):
            problem = "KVM and a readable virtio-capable kernel required"
        elif os.geteuid() != 0:
            problem = "root required to preserve exported file ownership"
        elif any(not shutil.which(p) for p in ("docker", "musl-gcc", "mkfs.ext4", "debugfs", "e2fsck")):
            problem = "Docker, musl-gcc and e2fsprogs required"
        elif subprocess.run(["docker", "info"], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL).returncode:
            problem = "Docker daemon unavailable"
        if problem:
            if os.environ.get("VIRTIO_BZIMAGE") or os.environ.get("BZIMAGE_PATH"):
                raise RuntimeError(problem)
            raise unittest.SkipTest(problem)
        shell.KERNEL = KERNEL
        cls.context = Path(tempfile.mkdtemp(prefix="svmm-workload-context-"))
        cls.addClassCleanup(shutil.rmtree, cls.context)
        cls.image = "svmm-workload-e2e:%d" % os.getpid()
        cls.addClassCleanup(subprocess.run, ["docker", "image", "rm", "-f", cls.image],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["musl-gcc", "-static", "-O2", "-Wall", "-Wextra",
                        str(Path(ROOT) / "tests/fixtures/workload-probe.c"),
                        "-o", str(cls.context / "probe")], check=True)
        (cls.context / "work").mkdir()
        (cls.context / "work/seed").write_text("from-image\n")
        (cls.context / "run/app").mkdir(parents=True)
        (cls.context / "run/app/seed").write_text("runtime-from-image\n")
        (cls.context / "work/seed-link").symlink_to("seed")
        (cls.context / "passwd").write_text(
            "root:x:0:0:root:/:/bin/sh\napp:x:1000:1001:app:/work:/bin/false\n")
        with tarfile.open(cls.context / "fifo.tar", "w") as archive:
            fifo = tarfile.TarInfo("work/pipe")
            fifo.type = tarfile.FIFOTYPE
            fifo.uid, fifo.gid, fifo.mode = 1000, 1001, 0o666
            archive.addfile(fifo)
        (cls.context / "group").write_text(
            "root:x:0:\nappgroup:x:1001:\nextra:x:1002:app\n")
        (cls.context / "Dockerfile").write_text(
            'FROM scratch\nCOPY probe /probe\nCOPY passwd /etc/passwd\n'
            'COPY group /etc/group\nCOPY --chown=1000:1001 work /work\n'
            'COPY --chown=1000:1001 run/app /run/app\nADD fifo.tar /\n'
            'USER app:appgroup\nWORKDIR /work\nENV TEST_VALUE=default\n'
            'ENTRYPOINT ["/probe", "inspect"]\nCMD ["default command"]\n')
        subprocess.run(["docker", "build", "--network=none", "-q", "-t", cls.image,
                        str(cls.context)], check=True, capture_output=True)
        subprocess.run(["make", "-s", "all", "initramfs"], cwd=ROOT, check=True)

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="svmm-workload-"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.disk = self.tmp / "rootfs.ext4"

    def build(self, *options, command=()):
        argv = [sys.executable, BUILDER, "--image", self.image, "--output", str(self.disk),
                "--size-mb", "64", *options]
        if command:
            argv += ["--", *command]
        result = subprocess.run(argv, cwd=ROOT, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode(errors="replace"))
        return result

    def boot(self, *extra, restore=None, tty=False, prefix=()):
        result = subprocess.run(["make", "-s", "workload-initramfs"], cwd=ROOT,
                                capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        args = ["--disk", str(self.disk), "--workload", *extra]
        base = ["run", "--restore", restore] if restore else ["run", "--kernel", KERNEL]
        if not restore:
            args += ["--initrd", INITRD, "--mem", "256"]
        guest = Guest(args=args, base_args=base, tty=tty, prefix=prefix)
        self.addCleanup(guest.close)
        return guest

    def finish(self, guest, code=7):
        self.assertEqual(guest.wait(30), code, (guest.output + guest.stderr)[-6000:].decode(errors="replace"))
        self.assertIn(b"reason=workload_exit", guest.stderr)
        check = subprocess.run(["e2fsck", "-fn", str(self.disk)], capture_output=True)
        self.assertEqual(check.returncode, 0, (check.stdout + check.stderr).decode(errors="replace"))
        return guest.output

    def read_file(self, path):
        return subprocess.run(["debugfs", "-R", "cat " + path, str(self.disk)],
                              capture_output=True, check=True).stdout

    def test_scratch_image_preserves_runtime_metadata_and_disk_writes(self):
        self.build("--env", "TEST_VALUE=" + ENV_VALUE, command=(SPECIAL, ""))
        output = self.finish(self.boot())
        for index, value in enumerate(("/probe", "inspect", SPECIAL, "")):
            self.assertIn(("arg%d=%s" % (index, value.encode().hex())).encode(), output)
        self.assertIn(("env=" + ENV_VALUE.encode().hex()).encode(), output)
        self.assertIn(b"cwd=2f776f726b", output)
        self.assertIn(b"identity=1000:1001", output)
        self.assertIn(b"seed=66726f6d2d696d6167650a", output)
        self.assertEqual(self.read_file("/work/result.txt"), b"persisted-from-guest\n")

    def test_entrypoint_override_and_signal_exit_status(self):
        self.build("--entrypoint", "/probe", command=("signal",))
        self.finish(self.boot(), 128 + signal.SIGTERM)

    def test_missing_executable_returns_127(self):
        self.build("--entrypoint", "/missing-program")
        self.finish(self.boot(), 127)

    def test_entrypoint_override_clears_image_default_cmd(self):
        self.build("--entrypoint", "/probe")
        output = self.finish(self.boot())
        self.assertIn(b"arg0=2f70726f6265", output)
        self.assertNotIn(b"arg1=", output)

    def test_standard_device_aliases_reach_application_stdio(self):
        self.build("--entrypoint", "/probe", command=("stdio",))
        guest = self.boot()
        guest.expect(b"STDIO_WAITING", 30)
        guest.send(b"x\n")
        output = self.finish(guest)
        for marker in (b"STDOUT_ALIAS_OK", b"STDERR_ALIAS_OK", b"FD_ALIAS_OK",
                       b"STDIN_ALIAS_OK"):
            self.assertIn(marker, output)

    def test_fifo_configuration_fails_promptly(self):
        self.build()
        commands = self.tmp / "fifo.commands"
        commands.write_text("cd /.svmm\nrm workload.bin\nmknod workload.bin p\n")
        result = subprocess.run(["debugfs", "-w", "-f", str(commands), str(self.disk)],
                                check=True, capture_output=True)
        self.assertIn(b"Allocated inode", result.stdout)
        guest = self.boot()
        try:
            guest.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.fail("guest init blocked opening FIFO workload configuration")
        self.finish(guest, 125)

    def test_image_run_directory_preserves_nonroot_ownership_and_contents(self):
        self.build("--entrypoint", "/probe", command=("runtime",))
        output = self.finish(self.boot())
        self.assertIn(b"RUN_DIRECTORY_PRESERVED", output)
        self.assertEqual(self.read_file("/run/app/writable"), b"runtime-written\n")

    def test_fifo_permissions_allow_other_user_io(self):
        self.build("--entrypoint", "/probe", "--user", "2000:2000", command=("fifo",))
        self.assertIn(b"FIFO_IO_OK", self.finish(self.boot()))

    def test_sigterm_cleans_temporary_docker_container(self):
        # Pause the real Docker export client so cancellation happens while
        # the builder owns a container. No Docker or subprocess mocks.
        argv = [sys.executable, BUILDER, "--image", self.image,
                "--output", str(self.disk)]
        proc = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        export_pid = None
        export_directory = None
        export_container = None
        def container_ids():
            result = subprocess.run(
                ["docker", "ps", "-aq", "--filter", "label=svmm.workload-builder=true",
                 "--filter", f"name=^/{export_container}$"], capture_output=True, check=True)
            return result.stdout.decode().split()
        try:
            deadline = time.monotonic() + 20
            children = Path("/proc/%d/task/%d/children" % (proc.pid, proc.pid))
            while proc.poll() is None and time.monotonic() < deadline:
                try:
                    candidates = children.read_text().split()
                except FileNotFoundError:
                    break
                for candidate in candidates:
                    try:
                        command = Path("/proc/%s/cmdline" % candidate).read_bytes()
                        if b"\0export\0" in command:
                            os.kill(int(candidate), signal.SIGSTOP)
                            export_pid = int(candidate)
                            arguments = command.split(b"\0")
                            export_directory = Path(os.fsdecode(arguments[arguments.index(b"--output") + 1])).parent
                            export_container = os.fsdecode(arguments[-2])
                            break
                    except (FileNotFoundError, ProcessLookupError):
                        pass
                if export_pid is not None:
                    break
                time.sleep(0.001)
            self.assertIsNotNone(export_pid, "could not observe the real Docker export")
            self.assertTrue(container_ids(), "builder should own an export container")
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=20)
            # An unfixed builder leaves the paused child holding stdout open.
            try:
                os.kill(export_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate(timeout=20)
            self.assertEqual(container_ids(), [], "SIGTERM leaked the temporary container")
            self.assertFalse(self.disk.exists(), "cancelled build published an output")
        finally:
            if proc.poll() is None:
                proc.kill()
            if export_pid is not None:
                try:
                    os.kill(export_pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            proc.communicate(timeout=20)
            if export_container is not None:
                subprocess.run(["docker", "rm", "-f", "-v", export_container],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if export_directory is not None:
                shutil.rmtree(export_directory, ignore_errors=True)

    def test_unknown_user_is_setup_failure(self):
        self.build("--user", "missing-user")
        self.finish(self.boot(), 125)

    def test_named_user_has_supplementary_groups(self):
        self.build("--user", "app")
        output = self.finish(self.boot())
        self.assertIn(b"identity=1000:1001", output)
        self.assertIn(b"group=1002", output)

    def test_pid1_reaps_orphaned_processes(self):
        self.build("--entrypoint", "/probe", command=("orphan",))
        self.assertIn(b"ORPHAN_REAPED", self.finish(self.boot()))

    def test_existing_output_is_unchanged_and_temporary_container_is_cleaned(self):
        self.build()
        before = self.disk.stat()
        data = self.disk.read_bytes()
        result = subprocess.run([sys.executable, BUILDER, "--image", self.image,
                                 "--output", str(self.disk)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.disk.read_bytes(), data)
        self.assertEqual(self.disk.stat().st_ino, before.st_ino)
        remaining = subprocess.run(["docker", "ps", "-aq", "--filter",
                                    "label=svmm.workload-builder=true"], capture_output=True, check=True)
        self.assertEqual(remaining.stdout.strip(), b"")

    def test_corrupt_config_returns_125(self):
        self.build()
        invalid = self.tmp / "invalid"
        invalid.write_bytes(b"truncated")
        commands = self.tmp / "debugfs.commands"
        commands.write_text("rm /.svmm/workload.bin\nwrite %s /.svmm/workload.bin\n" % invalid)
        subprocess.run(["debugfs", "-w", "-f", str(commands), str(self.disk)],
                        check=True, capture_output=True)
        self.finish(self.boot(), 125)

    def test_guest_poweroff_without_report_fails(self):
        self.build("--entrypoint", "/probe", "--user", "0:0", command=("poweroff",))
        guest = self.boot()
        self.assertEqual(guest.wait(30), 125, guest.stderr.decode(errors="replace"))
        self.assertNotIn(b"reason=workload_exit", guest.stderr)

    def test_invalid_exit_messages_do_not_end_vm(self):
        self.build("--entrypoint", "/probe", "--user", "0:0", command=("invalid-report",))
        output = self.finish(self.boot())
        self.assertIn(b"INVALID_REPORT_IGNORED", output)


    def snapshots(self):
        previous = os.environ.get("SVMM_SNAPSHOT_DIR")
        os.environ["SVMM_SNAPSHOT_DIR"] = str(self.tmp / "snapshots")
        def restore_environment():
            if previous is None:
                os.environ.pop("SVMM_SNAPSHOT_DIR", None)
            else:
                os.environ["SVMM_SNAPSHOT_DIR"] = previous
        self.addCleanup(restore_environment)

    def test_http_workload_is_reachable_over_tap(self):
        from test_virtio_net import NetNS
        self.build("--entrypoint", "/probe", command=("http",))
        ns = NetNS(self, "workload")
        ns.sh("ip tuntap add tap0 mode tap && ip addr add 192.168.100.1/24 dev tap0 && "
              "ip link set tap0 up")
        guest = self.boot("--tap", "tap0", prefix=ns.prefix)
        guest.expect(b"HTTP_READY", 30)
        request = subprocess.run(
            [*ns.prefix, sys.executable, "-c",
             "import http.client; c=http.client.HTTPConnection('192.168.100.2',8080,timeout=10); "
             "c.request('GET','/'); r=c.getresponse(); assert r.status==200; "
             "import sys; sys.stdout.buffer.write(r.read()); c.close()"],
            capture_output=True, timeout=15)
        self.assertEqual(request.returncode, 0, request.stderr.decode(errors="replace"))
        self.assertEqual(request.stdout, b"svmm-http-42\n")
        self.finish(guest)

    def test_warm_snapshot_preserves_application_state_and_exit_code(self):
        self.snapshots()
        self.build("--entrypoint", "/probe", command=("wait",))
        guest = self.boot("--warm-save", "application", tty=True)
        guest.expect(b"WAITING_FOR_STATE", 30)
        guest.send(b"s\n")
        guest.expect(b"SNAPSHOT_READY 73", 15)
        guest.proc.send_signal(signal.SIGUSR2)
        self.assertEqual(guest.wait(60), 0, guest.stderr.decode(errors="replace"))
        self.assertIn(b"warm snapshot 'application' saved", guest.stderr)
        resumed = self.boot(restore="application", tty=True)
        resumed.send(b"q\n")
        resumed.expect(b"RESUMED_STATE 74", 30)
        self.finish(resumed)
        self.assertNotIn(b"Linux version", resumed.output)
        self.assertEqual(self.read_file("/work/snapshot-result"), b"74\n")

    def test_cold_snapshot_restores_rootfs_and_launch_configuration(self):
        self.snapshots()
        self.build()
        saved = subprocess.run([VMM, "snapshot", "save", "--name", "application",
                                "--disk", str(self.disk)], capture_output=True)
        self.assertEqual(saved.returncode, 0, saved.stderr.decode(errors="replace"))
        changed = self.tmp / "changed"
        changed.write_text("changed-on-host\n")
        commands = self.tmp / "change.commands"
        commands.write_text("rm /work/seed\nwrite %s /work/seed\n" % changed)
        subprocess.run(["debugfs", "-w", "-f", str(commands), str(self.disk)],
                        check=True, capture_output=True)
        output = self.finish(self.boot("--snapshot", "application"))
        self.assertIn(b"seed=66726f6d2d696d6167650a", output)

    def test_workload_runs_with_host_jail(self):
        self.build()
        guest = self.boot("--jail")
        self.finish(guest)
        self.assertIn(b"seccomp installed", guest.stderr)

    def test_dynamic_ubuntu_workload(self):
        # Exercise a real distro's dynamic loader and libc as well as scratch.
        available = subprocess.run(["docker", "image", "inspect", "ubuntu:24.04"],
                                   capture_output=True)
        if available.returncode:
            self.skipTest("optional local ubuntu:24.04 image unavailable")
        self.build("--image", "ubuntu:24.04", "--size-mb", "192",
                   "--entrypoint", "/bin/sh", "--user", "0:0", "--workdir", "/",
                   "--env", "TEST_VALUE=ubuntu-value",
                   command=("-c", "printf 'DYNAMIC:%s\\n' \"$TEST_VALUE\"; "
                                 "echo ubuntu-persisted > /result; exit 7"))
        output = self.finish(self.boot())
        self.assertIn(b"DYNAMIC:ubuntu-value", output)
        self.assertEqual(self.read_file("/result"), b"ubuntu-persisted\n")



if __name__ == "__main__":
    unittest.main()

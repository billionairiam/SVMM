"""Single-command OCI workloads, exercised with real Docker and KVM.

Failures: no boot from --image, missing actual pull, lost arguments/identity,
cwd-dependent assets, leaked runtime disks, swallowed status/signals/snapshots,
and invalid CLI combinations touching a user disk. No Docker/KVM mocks.
"""
import gzip
import hashlib
import http.server
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tarfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_workload as workload
from test_workload import KERNEL, ENV_VALUE, SPECIAL
from test_initramfs_shell import Guest, ROOT, VMM


class RunImageTest(unittest.TestCase):
    # Reuse the real scratch-image fixture, without inheriting its test methods.
    setUpClass = classmethod(workload.WorkloadTest.setUpClass.__func__)
    setUp = workload.WorkloadTest.setUp

    def env(self):
        result = dict(os.environ)
        result["SVMM_KERNEL"] = KERNEL
        result["TMPDIR"] = str(self.tmp)
        return result

    def run_image(self, *args, image=None, env=None):
        return subprocess.run([VMM, "run", "--image", image or self.image, *args],
                              cwd=self.tmp, env=env or self.env(),
                              stdin=subprocess.DEVNULL, capture_output=True, timeout=90)

    def assert_finished(self, result, code=7):
        self.assertEqual(result.returncode, code,
                         (result.stdout + result.stderr)[-5000:].decode(errors="replace"))
        self.assertIn(b"reason=workload_exit", result.stderr)
        self.assertEqual(list(self.tmp.iterdir()), [], "temporary runtime disk leaked")

    def test_kernel_is_auto_selected_when_not_explicitly_configured(self):
        # This machine's compatible boot kernel is independently exercised by
        # the low-level E2E. Other environments may provide a packaged image.
        if not (Path("/boot/config-6.18.0").is_file() or
                (Path(ROOT) / "images/bzImage").is_file()):
            self.skipTest("no known packaged/boot guest kernel for auto-selection")
        environment = self.env()
        environment.pop("SVMM_KERNEL", None)
        self.assert_finished(self.run_image(env=environment))

    def test_explicit_kernel_overrides_invalid_environment_default(self):
        environment = self.env()
        environment["SVMM_KERNEL"] = "/nonexistent/kernel"
        self.assert_finished(self.run_image("--kernel", KERNEL, env=environment))

    def test_bad_kernel_fails_before_attempting_image_pull(self):
        result = self.run_image("--kernel", "/nonexistent/guest-kernel",
                                image="127.0.0.1:1/do-not-pull:latest")
        self.assertEqual(result.returncode, 125, result.stderr.decode(errors="replace"))
        self.assertIn(b"guest kernel is not readable", result.stderr)
        self.assertNotIn(b"SVMM_METRIC", result.stderr)
        self.assertEqual(list(self.tmp.iterdir()), [])

    def test_conflicting_flags_do_not_touch_user_disk(self):
        self.disk.write_bytes(b"preserve-user-disk")
        for extra in (("--disk", str(self.disk)), ("--snapshot", "saved"),
                      ("--restore", "saved")):
            with self.subTest(extra=extra):
                result = self.run_image(*extra)
                self.assertEqual(result.returncode, 2, result.stderr.decode(errors="replace"))
                self.assertIn(b"--image cannot be combined", result.stderr)
                self.assertNotIn(b"SVMM_METRIC", result.stderr)
                self.assertEqual(self.disk.read_bytes(), b"preserve-user-disk")
        result = subprocess.run([VMM, "run", "--kernel", KERNEL, "--env", "KEY=value"],
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"require --image", result.stderr)

    def test_native_option_values_and_grouped_short_flags_keep_their_meaning(self):
        environment = self.env()
        environment["SVMM_KERNEL"] = "/nonexistent/default-kernel"
        for arguments, expected in (
            (("-jk/nonexistent/explicit-kernel",), b"/nonexistent/explicit-kernel"),
            (("--cmdline", "--kernel"), b"/nonexistent/default-kernel"),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_image(*arguments, env=environment)
                self.assertEqual(result.returncode, 125, result.stderr.decode(errors="replace"))
                self.assertIn(expected, result.stderr)
                self.assertNotIn(b"SVMM_METRIC", result.stderr)
        self.assertEqual(list(self.tmp.iterdir()), [])

    def test_auto_selection_skips_kernel_without_gzip_initramfs_support(self):
        import shutil
        config = Path("/boot/config-6.18.0")
        if not config.is_file() or (Path(ROOT) / "images/bzImage").is_file():
            self.skipTest("isolated boot-kernel fixture requires local kernel config")
        boot = self.tmp / "boot"
        runtime = self.tmp / "runtime"
        boot.mkdir()
        runtime.mkdir()
        shutil.copyfile(KERNEL, boot / "vmlinuz-compatible")
        (boot / "config-compatible").write_text(config.read_text())
        (boot / "vmlinuz-incompatible").write_bytes(b"not-bootable-with-this-runtime")
        (boot / "config-incompatible").write_text(
            config.read_text().replace("CONFIG_RD_GZIP=y", "# CONFIG_RD_GZIP is not set"))
        os.utime(boot / "vmlinuz-compatible", (1, 1))
        os.utime(boot / "vmlinuz-incompatible", (2, 2))
        environment = self.env()
        environment.pop("SVMM_KERNEL", None)
        environment["TMPDIR"] = str(runtime)
        # Mount changes are private to this process tree, never the real /boot.
        result = subprocess.run(
            ["unshare", "--mount", "--propagation", "private", "sh", "-c",
             'mount --bind "$1" /boot || exit; shift; exec "$@"',
             "svmm-kernel-fixture", str(boot), VMM, "run", "--image", self.image],
            env=environment, stdin=subprocess.DEVNULL, capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 7,
                         (result.stdout + result.stderr)[-4000:].decode(errors="replace"))
        self.assertIn(b"reason=workload_exit", result.stderr)
        self.assertEqual(list(runtime.iterdir()), [])

    def test_one_command_runs_image_defaults_from_another_directory(self):
        result = self.run_image()
        self.assert_finished(result)
        self.assertIn(b"identity=1000:1001", result.stdout)
        self.assertIn(b"arg2=64656661756c7420636f6d6d616e64", result.stdout)

    def test_first_run_builds_missing_guest_runtime_from_bundled_assets(self):
        package = self.tmp / "package"
        (package / "bin").mkdir(parents=True)
        shutil.copy2(VMM, package / "bin/svmm")
        for relative in ("Cargo.toml", "Cargo.lock", "rootfs/build-workload-initramfs.sh", "Makefile",
                         *(str(path.relative_to(ROOT)) for path in Path(ROOT, "src-rust").rglob("*.rs"))):
            destination = package / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(Path(ROOT) / relative, destination)
        runtime = self.tmp / "runtime"
        runtime.mkdir()
        environment = self.env()
        environment["TMPDIR"] = str(runtime)
        environment.pop("SVMM_INITRD", None)
        result = subprocess.run([str(package / "bin/svmm"), "run", "--image", self.image],
                                cwd=self.tmp, env=environment, stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 7,
                         (result.stdout + result.stderr)[-5000:].decode(errors="replace"))
        self.assertIn(b"reason=workload_exit", result.stderr)
        self.assertTrue((package / "build/workload-initramfs.cpio.gz").is_file())
        self.assertEqual(list(runtime.iterdir()), [])

    def test_overrides_and_vm_options_reach_real_guest(self):
        result = self.run_image("--entrypoint", "/probe", "--env", "TEST_VALUE=" + ENV_VALUE,
                                "--user", "1000:1001", "--workdir", "/work", "--mem", "128",
                                "--", "inspect", SPECIAL, "")
        self.assert_finished(result)
        self.assertIn(("env=" + ENV_VALUE.encode().hex()).encode(), result.stdout)
        self.assertIn(("arg2=" + SPECIAL.encode().hex()).encode(), result.stdout)
        self.assertIn(b"arg3=\r\n", result.stdout)
        self.assertIn(b"guest_memory_bytes=134217728", result.stderr)

    def test_one_command_preserves_host_jail(self):
        result = self.run_image("--jail")
        self.assert_finished(result)
        self.assertIn(b"seccomp installed", result.stderr)

    def test_host_stop_reaches_vm_and_cleans_runtime(self):
        prefix = ["env", "SVMM_KERNEL=" + KERNEL, "TMPDIR=" + str(self.tmp)]
        guest = Guest(base_args=["run", "--image", self.image],
                      args=["--entrypoint", "/probe", "--", "wait"], prefix=prefix)
        self.addCleanup(guest.close)
        guest.expect(b"WAITING_FOR_STATE", 60)
        guest.proc.send_signal(signal.SIGTERM)
        self.assertEqual(guest.wait(20), 0, guest.stderr.decode(errors="replace"))
        self.assertIn(b"reason=host_stop", guest.stderr)
        self.assertEqual(list(self.tmp.iterdir()), [])

    def test_warm_save_signal_reaches_vm_and_snapshot_survives_temp_cleanup(self):
        runtime = self.tmp / "runtime"
        runtime.mkdir()
        snapshots = self.tmp / "snapshots"
        prefix = ["env", "SVMM_KERNEL=" + KERNEL, "TMPDIR=" + str(runtime),
                  "SVMM_SNAPSHOT_DIR=" + str(snapshots)]
        guest = Guest(base_args=["run", "--image", self.image],
                      args=["--warm-save", "application", "--entrypoint", "/probe",
                            "--", "wait"], prefix=prefix, tty=True)
        self.addCleanup(guest.close)
        guest.expect(b"WAITING_FOR_STATE", 60)
        guest.send(b"s\n")
        guest.expect(b"SNAPSHOT_READY 73", 15)
        guest.proc.send_signal(signal.SIGUSR2)
        self.assertEqual(guest.wait(60), 0, guest.stderr.decode(errors="replace"))
        self.assertTrue((snapshots / "application/disk.img").is_file())
        self.assertEqual(list(runtime.iterdir()), [])
        with self.disk.open("wb") as f:
            f.truncate(64 * 1024 * 1024)
        resumed = Guest(base_args=["run", "--restore", "application"],
                        args=["--disk", str(self.disk), "--workload"], tty=True, prefix=prefix)
        self.addCleanup(resumed.close)
        resumed.send(b"q\n")
        resumed.expect(b"RESUMED_STATE 74", 30)
        self.assertEqual(resumed.wait(30), 7, resumed.stderr.decode(errors="replace"))

    def test_missing_local_image_is_pulled_from_real_registry_and_run(self):
        # Serve an actual image as Registry v2 content. Docker performs a real
        # pull against localhost; no external registry or mock Docker executable.
        archive_path = self.tmp / "image.tar"
        subprocess.run(["docker", "save", "-o", str(archive_path), self.image], check=True)
        blobs = {}
        def blob(data):
            digest = "sha256:" + hashlib.sha256(data).hexdigest()
            blobs[digest] = data
            return {"digest": digest, "size": len(data)}
        with tarfile.open(archive_path) as archive:
            saved = json.load(archive.extractfile("manifest.json"))[0]
            config = blob(archive.extractfile(saved["Config"]).read())
            config["mediaType"] = "application/vnd.docker.container.image.v1+json"
            layers = []
            for name in saved["Layers"]:
                data = archive.extractfile(name).read()
                if not data.startswith(b"\x1f\x8b"):
                    data = gzip.compress(data, mtime=0)
                layer = blob(data)
                layer["mediaType"] = "application/vnd.docker.image.rootfs.diff.tar.gzip"
                layers.append(layer)
        archive_path.unlink()
        manifest = json.dumps({"schemaVersion": 2,
                               "mediaType": "application/vnd.docker.distribution.manifest.v2+json",
                               "config": config, "layers": layers}).encode()
        requested = []
        class Registry(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass
            def do_HEAD(self):
                self.reply(False)
            def do_GET(self):
                self.reply(True)
            def reply(self, body):
                requested.append(self.path)
                if self.path.rstrip("/") == "/v2":
                    data, media = b"{}", "application/json"
                elif self.path.startswith("/v2/fixture/manifests/"):
                    data, media = manifest, "application/vnd.docker.distribution.manifest.v2+json"
                elif self.path.startswith("/v2/fixture/blobs/"):
                    digest = self.path.rsplit("/", 1)[-1]
                    if digest not in blobs:
                        self.send_error(404)
                        return
                    data, media = blobs[digest], "application/octet-stream"
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", media)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Docker-Distribution-Api-Version", "registry/2.0")
                self.send_header("Docker-Content-Digest",
                                 "sha256:" + hashlib.sha256(data).hexdigest())
                self.end_headers()
                if body:
                    self.wfile.write(data)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Registry)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        reference = "127.0.0.1:%d/fixture:latest" % server.server_port
        self.addCleanup(subprocess.run, ["docker", "image", "rm", "-f", reference],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        local = subprocess.run(["docker", "image", "inspect", reference], capture_output=True)
        self.assertNotEqual(local.returncode, 0, "test requires a missing local image")
        result = self.run_image(image=reference)
        self.assert_finished(result)
        self.assertTrue(any("/v2/fixture/manifests/" in path for path in requested),
                        "Docker did not pull the image manifest from the registry")


if __name__ == "__main__":
    unittest.main()

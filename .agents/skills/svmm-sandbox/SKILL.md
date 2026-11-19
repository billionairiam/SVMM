---
name: svmm-sandbox
description: Use when a user asks to run a command, script, build, or test inside SVMM as an isolated VM, or to run an OCI image as an agent task sandbox.
---

# Run tasks in an SVMM sandbox

Use the sandbox for the task command and collect its output and exit status. Use [svmm-oci](../svmm-oci/SKILL.md) if the task is to change or debug the SVMM image runner itself.

## Prepare the run

- Locate the SVMM checkout and `bin/svmm`. If it has yet to be built, follow [Quick start](../../../README.md#quick-start) and run `make all workload-initramfs` from the checkout. The host needs access to `/dev/kvm`, a compatible guest bzImage, Docker and root privileges for the OCI path. If SVMM cannot start, report the blocker and keep the task command inside the requested boundary.
- Use the task's linux/amd64 image, or select an existing image with the needed tools. Pass program and arguments separately. `--entrypoint EXE` selects the guest executable and clears the image's default command. Put only the guest arguments after `--`.
- The guest starts from an independent rootfs. For a small local script, pass its contents as a single guest argument to the chosen interpreter, using the host's argument-vector API or shell quoting. For a directory of inputs, use an OCI image containing the needed tools and copy in the task files so execution happens after the VM boots. The host working directory does not appear in the guest.
- Leave `--tap` unset for a guest without a NIC. Docker can still pull images on the host. When the task also requires offline image preparation, verify the selected image is cached before launch. For a networked task, use its configured TAP and netns.

## Run and collect the result

This example assumes the launcher already has root privileges. Use the configured guest kernel or add `--kernel PATH`. Use an explicit time limit and give SVMM time to clean up after SIGTERM.

```sh
status=0
timeout --signal=TERM --kill-after=10s 90s bin/svmm run \
  --image ubuntu:24.04 --mem 256 --jail --entrypoint /bin/sh \
  -- -c 'printf "SVMM_TASK_RESULT=ok\n"; exit 7' </dev/null || status=$?
printf 'launcher_status=%s\n' "$status"
```

- Expect the result marker on stdout, `reason=workload_exit` on host stderr, and status `7` in this example. Guest stdout and stderr share the serial stream. Capture it separately from the VMM diagnostics. Treat unexpected boot, timeout, or launcher failures as infrastructure results. In particular, distinguish a deadline expiry from an application that returns 124 using the VMM completion metric.
- `--jail` applies to ordinary KVM. If the task specifies TDX, use `--tdx` with the required firmware and kernel and omit `--jail`. See [svmm-tdx](../svmm-tdx/SKILL.md) for hardware prerequisites. If the task depends on host cgroup limits, confirm that the jailer reports they were applied.
- `run --image` cleans up its writable disk after the run. For artifacts that must survive, use `image-build --output PATH` with a task-specific ext4 path, then `run --workload --disk PATH` with an explicit kernel and `build/workload-initramfs.cpio.gz`. After the VM exits, read a text artifact with `debugfs -R 'cat /path/to/result' PATH`. See [manual workload build and run](../../../docs/guide.md#手动构建和运行).

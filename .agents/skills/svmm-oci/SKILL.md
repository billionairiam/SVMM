---
name: svmm-oci
description: Use when adding or debugging SVMM image pulling, Docker export, ext4 rootfs preparation, guest PID 1, or workload exit and cleanup behavior.
---

# SVMM OCI workloads

Trace failures across these boundaries.

1. [src-rust/main.rs](../../../src-rust/main.rs) and [src-rust/oci.rs](../../../src-rust/oci.rs) select the kernel and initramfs, inspect or pull the linux/amd64 image, export it into a writable ext4 disk, and start the VM. Use `image-build` to isolate image preparation and keep the disk for inspection.
2. [src-rust/guest.rs](../../../src-rust/guest.rs) reads the workload configuration, mounts the rootfs, starts the app with the chosen identity, and reports completion. Build this init with `make workload-initramfs`.
3. [src-rust/runtime.rs](../../../src-rust/runtime.rs) validates the guest completion message and returns its status. Check the `reason=workload_exit` metric together with the process status.

Preserve the distinction between a command after `--` (replaces CMD) and `--entrypoint` (changes ENTRYPOINT and clears the default CMD). Keep arguments as an array through serialization and exec. The image coordinator owns the temporary disk and container cleanup, including when the VM uses `--jail`. For cancellation, identify the container that the builder created by its exact name, since different runs can share the same image ID.

With a usable KVM host, Docker daemon, and root privileges to preserve image ownership, run the test that exercises the changed boundary.

```sh
VIRTIO_BZIMAGE=/path/to/virtio-bzImage python3 -m unittest -v \
  tests.test_run_image.RunImageTest.test_one_command_runs_image_defaults_from_another_directory \
  tests.test_workload.WorkloadTest.test_sigterm_cleans_temporary_docker_container
```

For pull regressions, use `tests.test_run_image.RunImageTest.test_missing_local_image_is_pulled_from_real_registry_and_run`, which serves a local Registry v2 fixture. For ordinary CLI and test prerequisites, see [svmm-e2e](../svmm-e2e/SKILL.md).

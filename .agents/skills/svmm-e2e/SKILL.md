---
name: svmm-e2e
description: Use when verifying an SVMM change, choosing a focused test, or debugging boot, TAP, network namespace, or jailer regressions.
---

# SVMM E2E

Read [AGENTS.md](../../../AGENTS.md) for the project's test policy. Run commands from the repository root. Build the Rust host binaries with `make all`. Use the affected path below to choose the smallest useful E2E.

| Changed path | Test entry point | Behavior to check |
| --- | --- | --- |
| Boot, serial, or CLI | `tests.test_sandbox_cli`, `tests.test_initramfs_shell` | Guest reaches the shell, terminal state is restored, and the exit status matches the exit path |
| Virtio queue or block | `tests.test_virtq_demo`, `tests.test_virtio_blk` | Queue completion and guest writes that persist across boots |
| TAP or netns | `tests.test_virtio_net`, `tests.test_network_policy` | Guest and host exchange traffic through the requested TAP in the correct namespace |
| Jailer | `tests.test_jailer` | cgroup and rlimits take effect, worker threads have seccomp, and disk, TAP, and shutdown still work |
| OCI, TDX, or snapshots | See the specialized skills | [OCI](../svmm-oci/SKILL.md), [TDX](../svmm-tdx/SKILL.md), [snapshots](../svmm-snapshots/SKILL.md) |

For example, run the serial E2E with a known bootable kernel.

```sh
BZIMAGE_PATH=/path/to/bzImage python3 -m unittest -v tests.test_initramfs_shell
```

- `BZIMAGE_PATH` supplies the ordinary guest kernel. Device and workload tests prefer `VIRTIO_BZIMAGE` when set. Use a kernel with the relevant drivers built in, including virtio-mmio and ext4 for disk tests. Check which fixture the selected test loads.
- Confirm access to `/dev/kvm`. TAP and netns tests also need `/dev/net/tun`, `ip`, and privileges to create temporary interfaces and namespaces. Network policy E2E also needs `nft`. Jailer E2E uses cgroup v2. Let the test fixtures create and clean up their own resources.
- Check the last `SVMM_METRIC` stage on host stderr, the serial output on stdout, and the actual process status. With missing prerequisites, report any skipped or blocked hardware cases separately from passing checks. `make test` runs the broad suite. `make test-tdx` runs the TDX hardware suite.

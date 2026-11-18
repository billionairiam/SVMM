---
name: svmm-snapshots
description: Use when changing SVMM cold or warm snapshot formats, disk restore, VM state validation, or snapshot save and resume behavior.
---

# SVMM snapshots

Work from [snapshot.rs](../../../src-rust/snapshot.rs) for manifests, hashes, and file publication, [vmstate.rs](../../../src-rust/vmstate.rs) for warm state sections, and [runtime.rs](../../../src-rust/runtime.rs) for pausing and resuming the VM. The manifests and warm state share a format with the C implementation on `stage11`.

- Cold restore replaces the workspace disk before a new boot. For ordinary KVM warm restore, use the same host and match the snapshot's disk and TAP configuration. Use `--workload` for a warm restore of an OCI-backed workload.
- Use `SVMM_SNAPSHOT_DIR` with a fresh temporary directory for manual tests. Inspect `snapshot validate` and restore together when changing validation. A malformed snapshot must fail before replacing the destination disk. If a warm save fails, the guest should resume without a partial snapshot being published.
- For format changes, check an existing snapshot as well as a new round trip. For warm state, verify that the resumed guest keeps its process state, disk contents, and TAP behavior where applicable.

```sh
BZIMAGE_PATH=/path/to/virtio-bzImage python3 -m unittest -v \
  tests.test_snapshot.SnapshotTest.test_corrupt_snapshots_are_rejected \
  tests.test_warm_snapshot.WarmSnapshotTest.test_session_resumes_from_pause_point \
  tests.test_warm_snapshot.WarmSnapshotValidationTest.test_corrupt_or_mismatched_snapshots_fail_closed
```

Choose the warm corruption test for section and hash validation and the resume test for lifecycle changes. See [svmm-e2e](../svmm-e2e/SKILL.md) for host prerequisites.

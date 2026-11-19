---
name: svmm-tdx
description: Use when changing or diagnosing SVMM native KVM TDX launch, TD-Shim, vCPU bring-up, high memory, shared virtio DMA, or TD workload completion.
---

# SVMM native KVM TDX

Read the [TDX requirements and limits](../../../docs/guide.md#stage-13在-intel-tdx-中运行-oci-镜像). Identify the host KVM TDX support, the exact guest bzImage and TD-Shim payload, and the requested vCPU count before interpreting a failure. For SMP, verify the shim includes the AP wake fix and that the guest kernel's `CONFIG_NR_CPUS` covers the requested CPUs.

- For launch errors, start with [machine.rs](../../../src-rust/machine.rs), [boot/tdx.rs](../../../src-rust/boot/tdx.rs) and [boot/td_shim.rs](../../../src-rust/boot/td_shim.rs). Check the last boot stage to distinguish preflight, memory import, and vCPU entry.
- For TAP and disk failures, follow the shared GPA and DMA checks in [memory.rs](../../../src-rust/memory.rs) and [devices.rs](../../../src-rust/devices.rs). Private TD memory is inaccessible to the device model. Preserve that boundary when changing device access.
- For multi-vCPU exit or shutdown, inspect [runtime.rs](../../../src-rust/runtime.rs). Completion may arrive on a secondary vCPU. Verify the exit status and that all vCPU threads stop.

```sh
SVMM_TEST_TDX_KERNEL=/path/to/tdx-bzImage \
SVMM_TEST_TD_SHIM=/path/to/td-shim.bin python3 -m unittest -v \
  tests.test_tdx.TdxTest.test_linux_reports_non_debug_tdx \
  tests.test_tdx.TdxTest.test_four_vcpus_run_workload_and_finish_on_secondary_cpu
```

Use the focused hardware case for the behavior you changed. The suite also covers AVX, 4 GiB guest memory, TAP, OCI, and firmware rejection. A local TDREPORT verifies the TD boot path. Remote attestation requires a Quote and a verifier. Record any hardware validation that was blocked or skipped.

# Native KVM TDX Implementation Plan

> Execute inline using superpowers:executing-plans. Follow AGENTS.md and add hardware E2E before implementation.

**Goal:** Boot a real non-debug Trust Domain and run an OCI image with its actual exit status.
**Architecture:** TD-Shim starts Linux through its 64-bit handoff. SVMM owns KVM, private memory and shared page conversion, and emulates the serial and block device interfaces.
**Tech stack:** C, Linux KVM TDX UAPI, TD-Shim, Python E2E, static guest init.
**Spec:** docs/superpowers/specs/2026-11-18-native-tdx-design.md

### 1. Measured cold boot
- Write hardware E2E using a static PID 1 which obtains TDREPORT with a known REPORTDATA, verifies DEBUG is off, then powers off. Add clean failure behavior for invalid TD arguments and firmware.
- Add TDX CLI and preflight. Add the backend in src/tdx.c and src/tdx.h and firmware handoff in src/boot/td-shim.c. Reuse src/boot/linux.c to validate bzImage and give the shim's zero page the Linux setup header.
- Validate TD-Shim metadata and build HOB with system memory, ACPI and payload. Create TD, import firmware and startup pages, finalize, and run the vCPU. Verify the hardware E2E.

### 2. Bring up shared I/O
- Test actual rootfs reads/writes, sync, and application exit status through a disk with the existing guest workload init before modifying device code.
- Add split IOAPIC in src/ioapic.c, route edge interrupts through MSI, and declare the TD blk resource as edge-triggered. Handle verified shared/private transitions and validate complete virtqueue DMA ranges.
- Complete workload via kernel-mediated byte PIO in TDX, with an independent decoder. Verify preserved writes and clean ext4.

### 3. Image integration
- Add E2E for --tdx --image with Docker and a static probe, verify cleanup and failure before Docker on invalid firmware.
- Wire TDX preflight and TDX-aware kernel selection through scripts/run-image.py. Reject unsupported network, jail, and snapshot combinations.
- Re-run affected hardware E2E and plain KVM/OCI tests. Review the branch and document the commands, local TDREPORT, measured inputs and host-visible I/O.

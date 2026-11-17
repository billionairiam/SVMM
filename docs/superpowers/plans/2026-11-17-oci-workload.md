# OCI Workload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Run an OCI image's application in SVMM and return its real exit status.
**Architecture:** Host Docker exports the flattened rootfs and image configuration into ext4. A separate static C guest init mounts it, sets up the application environment, supervises the process, and reports completion through a dedicated I/O port.
**Tech Stack:** C11, musl, Python 3, Docker, e2fsprogs, KVM.
**Spec:** docs/superpowers/specs/2026-11-17-oci-workload-design.md

## Global Constraints
- Linux x86_64, one image per VM, existing virtio-blk and virtio-net.
- Preserve ordinary run, user changes, current snapshot format and jail constraints.
- Tests first; real Docker/KVM E2E; run relevant tests only, as AGENTS.md requests.
- User authorized implementation and push when complete; execute inline in this session.

## Review Focus
- Host archive extraction must not follow links outside the temporary root.
- Non-root USER must be applied, including group membership and writable paths.
- Malformed/truncated configuration and missing exit reports must fail promptly.
- Missing shell/init in scratch images must not prevent startup.
- Saving/restoring an application must preserve memory and return the actual final status.

### Task 1: Image-to-VM application execution
**Files:** Create scripts/build-workload.py, guest/init.c, src/workload.h, tests/test_workload.py, tests/fixtures/workload-probe.c; modify Makefile, src/cli.c, src/sandbox.h, src/sandbox.c, src/vcpu.h, src/vcpu.c.
**Interfaces:** Builder CLI --image IMAGE --output PATH [--entrypoint PATH] [--env KEY=VALUE] [--workdir PATH] [--user USER] [--size-mb N] [-- COMMAND ...]; VM run --workload --disk PATH --initrd PATH; guest report port 0x604, 32-bit message 0x53564d00 | status.
- [ ] Write real Docker-to-KVM E2E for scratch probe argv/env/cwd/USER, disk writes and exit 7; converter refusal to overwrite; exec/signal failure; invalid configuration; ordinary guest shutdown without a report.
- [ ] Run those tests and observe failure because workload build/init/run support is absent.
- [ ] Implement host converter using Docker export, safe tar extraction, bounded binary config and atomic ext4 publication.
- [ ] Implement static guest init, bootstrap initramfs, --workload validation and exit status protocol.
- [ ] Run tests/test_workload.py core tests plus existing shell, disk and CLI E2E; require passing output.
- [ ] Commit only task files.

### Task 2: Network and snapshot integration
**Files:** tests/test_workload.py, tests/fixtures/workload-probe.c; relevant production files only for demonstrated defects.
**Interfaces:** Existing --tap/--netns, --warm-save and --restore with --workload, unchanged vmstate format.
- [ ] Add and run a real HTTP workload through an isolated TAP/netns; verify response body.
- [ ] Add and run warm snapshot of waiting application with live memory; resume and exit 7; check persisted files and clean ext4.
- [ ] Fix observed integration failures only after failing E2E, then run the full relevant workload test file.
- [ ] Commit task changes.

### Task 3: Documentation and final verification
**Files:** README.md, Makefile help, spec/plan progress.
**Interfaces:** Exact working build/run/test commands and documented limitations.
- [ ] Document dependencies, rootfs preparation, metadata overrides, return codes, storage and snapshot use.
- [ ] Run workload E2E and existing relevant shell/disk/CLI/warm snapshot/jail paths.
- [ ] Request one fresh whole-change code review; fix important findings with RED-to-GREEN E2E.
- [ ] Verify git diff --check and commit only task files, preserving user's serial.c and trace document.
- [ ] Push current stage11 branch and verify remote HEAD equals the final local commit.

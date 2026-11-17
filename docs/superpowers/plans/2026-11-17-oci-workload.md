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
- [x] Write real Docker-to-KVM E2E for scratch probe argv/env/cwd/USER, disk writes and exit 7; converter refusal to overwrite; exec/signal failure; invalid configuration; ordinary guest shutdown without a report.
- [x] Run those tests and observe failure because workload build/init/run support is absent.
- [x] Implement host converter using Docker export, safe tar extraction, bounded binary config and atomic ext4 publication.
- [x] Implement static guest init, bootstrap initramfs, --workload validation and exit status protocol.
- [x] Run tests/test_workload.py core tests plus existing shell, disk and CLI E2E; require passing output.
- [x] Commit only task files.

### Task 2: Network and snapshot integration
**Files:** tests/test_workload.py, tests/fixtures/workload-probe.c; relevant production files only for demonstrated defects.
**Interfaces:** Existing --tap/--netns, --warm-save and --restore with --workload, unchanged vmstate format.
- [x] Add and run a real HTTP workload through an isolated TAP/netns; verify response body.
- [x] Add and run warm snapshot of waiting application with live memory; resume and exit 7; check persisted files and clean ext4.
- [x] Fix observed integration failures only after failing E2E, then run the full relevant workload test file.
- [x] Commit task changes.

### Task 3: Documentation and final verification
**Files:** README.md, Makefile help, spec/plan progress.
**Interfaces:** Exact working build/run/test commands and documented limitations.
- [x] Document dependencies, rootfs preparation, metadata overrides, return codes, storage and snapshot use.
- [x] Run workload E2E and existing relevant shell/disk/CLI/warm snapshot/jail paths.
- [x] Request one fresh whole-change code review; fix important findings with RED-to-GREEN E2E.
- [x] Verify git diff --check and commit only task files, preserving user's serial.c and trace document.
- [x] Push current stage11 branch and verify remote HEAD equals the final local commit.

## 执行与验证记录（2026-11-17）

- 已实现镜像转换、静态 guest init、--workload、真实应用退出码、网络、jail 和冷热快照。
- 最终 `make test-workload`：21/21 通过，无跳过；内核 /boot/vmlinuz-6.18.0.bak，真实 Docker 与 KVM。
- 相关旧路径回归：12/12 通过，涵盖 shell 关机、磁盘持久化、CLI、热快照与 jail。
- 独立审阅发现的 stdio 别名、FIFO 配置阻塞、/run 覆盖和 entrypoint/CMD 问题均以真实 E2E 复现后修复。进一步把 FIFO 权限丢失及 SIGTERM 容器泄漏按用户可见影响视作需要修复的缺陷，也均 RED → GREEN；非 root 终端重开权限随 stdio E2E 修正。
- 采用当前 stage11 功能分支原位执行，保留用户 serial.c 与 trace 文档修改；不额外创建 worktree 或 PR。
- 测试中的孤儿进程探针曾错误地保留父进程，已改成真正的孤儿孙进程；没有为错误预期修改生产代码。
- 审阅边界：完整 OCI 生命周期、容器编排、跨架构、完整 TTY 和独立输出流、优雅宿主停止不属于本期；误用于这些场景会缺少对应语义。guest 特权退出报告不构成可信证明。没有添加归档资源配额；SIGKILL 无法保证临时资源清理。xattr/file capability 与外部 NSS 不保证兼容。上述兼容性边界及命令已记录在 README。
- 审阅后的修复由失败到通过的 E2E 与最终整组相关测试验证，未追加重复审阅；原有未提交修改均排除在提交之外。

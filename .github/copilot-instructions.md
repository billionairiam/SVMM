# Copilot instructions for SVMM

SVMM is a minimal x86 KVM virtual machine monitor written in C11, built step by step following the [Runra "sandbox from scratch" tutorial](https://runra.dev/zh/blog/sandbox-from-scratch-00-first-kvm-exit-io). README and code comments are written in Chinese; keep new docs/comments consistent with that.

## Branch-per-stage layout

Each tutorial stage lives on its own branch (`stage00`, `stage01`, `stage02`, `stage03`), each one building on the previous. Check `git branch --show-current` before working — file layout, binary names, and tests differ per stage:

- `stage00`: real-mode guest writes bytes to debug port `0xe9`; binary `build/stage-00-kvm-hello`.
- `stage01`: adds a serial (UART) device model (`src/serial.c`); binary moves to `bin/serial_console`.
- `stage02`/`stage03`: boot a Linux bzImage (`src/boot/linux.c`, `src/boot/acpi.c`), console/metrics, ablation variants, and (stage03) an initramfs BusyBox shell. These branches carry an `AGENTS.md` with testing rules (see below).

`build/` and `bin/` are gitignored; untracked artifacts from other stages may be present in the working tree — don't treat them as part of the current stage.

## Build and test (stage00)

```sh
make all     # compiles to build/stage-00-kvm-hello
make run     # runs the VM; prints exactly "hello,box" with NO trailing newline
make test    # sh tests/test_kvm_hello.sh
make clean
```

- Flags: `-std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE` — any warning breaks the build.
- There is one E2E test: it builds, runs the VM under `timeout 5`, and byte-compares stdout with `tests/golden/stage-00.txt` via `cmp`. It prints `SKIP` (exit 0) when not on Linux or when `/dev/kvm` isn't readable/writable, so a "passing" run may not have exercised the VM — check for `PASS:` in the output.
- Golden files must match guest output byte-for-byte. `stage-00.txt` intentionally has no trailing newline; don't let an editor add one.

## Architecture

`src/main.c` wires four modules in order and unwinds them via a single `goto done` cleanup path:

1. `kvm.c` (`struct kvm_context`): open `/dev/kvm`, check `KVM_GET_API_VERSION`, `KVM_CREATE_VM`, then `KVM_SET_TSS_ADDR` at `0xfffbd000` (required for real-mode guests on Intel; must not overlap guest RAM).
2. `memory.c` (`struct guest_memory`): `mmap` 64 KiB anonymous memory and register it as slot 0 at GPA 0 with `KVM_SET_USER_MEMORY_REGION`; `guest_memory_load` bounds-checks copies into guest RAM.
3. `guest_code.h`: raw 37-byte guest machine code (`mov al,imm` / `out 0xe9,al` ×9, then `hlt`) loaded at GPA 0.
4. `vcpu.c` (`struct vcpu`): `KVM_CREATE_VCPU`, `mmap` the `kvm_run` region (size from `KVM_GET_VCPU_MMAP_SIZE`), set real-mode regs (`CS.base=0`, `CS.selector=0`, `RIP=0`, `RFLAGS=0x2`), then loop on `KVM_RUN`.

The `vcpu_run` exit loop is the VMM's device model: `KVM_EXIT_IO` reads the byte at `run + io.data_offset` and writes it to the output fd; `KVM_EXIT_HLT` ends the run successfully. Any other exit reason, or any I/O that isn't a 1-byte, count-1 `OUT` to port `0xe9`, is a hard error. New devices/ports are added as cases in this loop.

## Code conventions

- Each module is a struct plus `*_init` / `*_destroy` functions. `init` first sets fields to sentinel values (`-1` fds, `NULL` pointers) so `destroy` is safe to call on partially initialized or never-initialized objects; `destroy` resets fields after releasing them. `main` also zero/sentinel-initializes structs before calling `init`.
- Functions return `0` on success and `-1` on failure, reporting errors at the failure site with `perror("<ioctl or syscall name>")` or `fprintf(stderr, ...)`. Callers just propagate.
- Retry `ioctl(KVM_RUN)` and `write` on `EINTR`.
- Headers use `#ifndef <NAME>_H` guards and forward-declare structs (`struct kvm_run;`) rather than including `<linux/kvm.h>` in public headers.
- Kernel-style formatting: 4-space indent, function opening brace on its own line, no braces on single-statement `if` bodies.

## Testing rules (from `AGENTS.md` on later stage branches)

- Do not write the implementation first and then pad it with unit tests that just assert constants or substrings to "pass".
- Prefer E2E tests that prove the feature actually runs in the VM. During development run only the E2E tests related to the change; run the full suite before opening a PR.
- If a component genuinely needs isolated testing, first enumerate the ways it can fail, then write the code/tests (a TDD variant).

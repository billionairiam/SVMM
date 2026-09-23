# Stage 02 Ablation Experiment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and execute reproducible compile-time ablations for every key Stage 02 Linux boot component, then commit raw measurements, summaries, logs, and a written report.

**Architecture:** A small `ablation.h` maps one compile-time macro to one disabled component, while Make builds every variant into isolated object and binary directories. The VMM emits stable `SVMM_METRIC` records, and a standard-library Python collector builds variants, runs two kernels, classifies outcomes, aggregates measurements, and writes CSV plus Markdown artifacts.

**Tech Stack:** C11, Linux KVM API, GNU Make, POSIX shell, Python 3 standard library, `/usr/bin/time`, CSV and Markdown.

**Spec:** `docs/superpowers/specs/2026-09-23-stage02-ablation-design.md`

## Global Constraints

- Each experiment variant removes exactly one component through a compile-time macro.
- Default `make` and `bin/linux_boot` must retain baseline behavior.
- Each variant must use isolated object, dependency, and binary directories.
- Never bypass a bounds check to make an ablated variant continue with invalid memory.
- One run may last at most 15 seconds; failure and timeout must not stop later runs.
- Primary kernel defaults to `/boot/vmlinuz-6.16.0`; compatibility kernel defaults to `/boot/vmlinuz-6.18.0.bak`.
- Performance measurements use 10 repetitions only for variants whose functional run reaches an identifiable kernel endpoint.
- The implementation uses only existing build tools and the Python 3 standard library.
- Preserve the existing `align_up()` implementation and the distinction between `kernel_alignment` and memory-size alignment.

## Review Focus

- A missing kernel or inaccessible `/dev/kvm` must fail before existing result files are replaced; Task 5 adds `test_prerequisites_preserve_results`.
- A timed-out child must be terminated and classified without leaving a live VMM; Task 5 adds `test_timeout_kills_process_group`.
- A VFS panic followed by timeout must classify as `panic`, while retaining the timeout flag; Task 5 adds `test_panic_takes_priority_over_timeout`.
- Variant builds must never reuse objects compiled with another macro; Task 1 adds `tests/test_ablation_build.sh` checks for one isolated object tree per variant.
- CSV paths containing spaces or commas must remain valid fields; Task 5 adds `test_csv_round_trip_with_special_path` using Python's `csv` module.

---

### Task 1: Compile-time configuration and isolated variant builds

**Files:**
- Create: `src/ablation.h`
- Create: `tests/test_ablation_build.sh`
- Modify: `Makefile`

**Interfaces:**
- Consumes: existing `SOURCES`, `CPPFLAGS`, and normal `bin/linux_boot` build.
- Produces: `SVMM_ABLATE_*_ENABLED` integer macros, `SVMM_VARIANT_NAME`, `make ablation`, `bin/ablation/<variant>/linux_boot`, and isolated `build/ablation/<variant>/` objects.

- [ ] **Step 1: Write the failing build-isolation test**

Create `tests/test_ablation_build.sh`:

```sh
#!/bin/sh
set -eu

variants='baseline no_cpuid fixed_32m no_boot_params no_e820 no_cmdline no_protected_mode no_uart'
make ablation
for variant in $variants; do
    test -x "bin/ablation/$variant/linux_boot"
    test -f "build/ablation/$variant/main.o"
    test -f "build/ablation/$variant/boot/linux.o"
done

tmp_object=$(mktemp)
trap 'rm -f "$tmp_object"' EXIT HUP INT TERM
if printf '#include "ablation.h"\n' |
   ${CC:-cc} -Isrc -DSVMM_ABLATE_CPUID -DSVMM_ABLATE_UART \
       -x c -c -o "$tmp_object" -; then
    echo 'multiple ablations were accepted' >&2
    exit 1
fi
```

- [ ] **Step 2: Run the test and confirm the target is absent**

Run: `sh tests/test_ablation_build.sh`

Expected: FAIL because Make has no `ablation` target.

- [ ] **Step 3: Add the compile-time configuration header**

Create `src/ablation.h` with include guards and this structure for all seven macros:

```c
#ifndef SVMM_ABLATION_H
#define SVMM_ABLATION_H

#ifdef SVMM_ABLATE_CPUID
#define SVMM_ABLATE_CPUID_ENABLED 1
#else
#define SVMM_ABLATE_CPUID_ENABLED 0
#endif
#ifdef SVMM_ABLATE_DYNAMIC_MEMORY
#define SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED 1
#else
#define SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED 0
#endif
#ifdef SVMM_ABLATE_BOOT_PARAMS
#define SVMM_ABLATE_BOOT_PARAMS_ENABLED 1
#else
#define SVMM_ABLATE_BOOT_PARAMS_ENABLED 0
#endif
#ifdef SVMM_ABLATE_E820
#define SVMM_ABLATE_E820_ENABLED 1
#else
#define SVMM_ABLATE_E820_ENABLED 0
#endif
#ifdef SVMM_ABLATE_CMDLINE
#define SVMM_ABLATE_CMDLINE_ENABLED 1
#else
#define SVMM_ABLATE_CMDLINE_ENABLED 0
#endif
#ifdef SVMM_ABLATE_PROTECTED_MODE
#define SVMM_ABLATE_PROTECTED_MODE_ENABLED 1
#else
#define SVMM_ABLATE_PROTECTED_MODE_ENABLED 0
#endif
#ifdef SVMM_ABLATE_UART
#define SVMM_ABLATE_UART_ENABLED 1
#else
#define SVMM_ABLATE_UART_ENABLED 0
#endif

#if SVMM_ABLATE_CPUID_ENABLED + SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED + \
    SVMM_ABLATE_BOOT_PARAMS_ENABLED + SVMM_ABLATE_E820_ENABLED + \
    SVMM_ABLATE_CMDLINE_ENABLED + SVMM_ABLATE_PROTECTED_MODE_ENABLED + \
    SVMM_ABLATE_UART_ENABLED > 1
#error "only one Stage 02 ablation may be enabled per binary"
#endif

#ifndef SVMM_VARIANT_NAME
#define SVMM_VARIANT_NAME "baseline"
#endif

#endif
```


- [ ] **Step 4: Add isolated Make targets**

Add the exact variant-to-flag mapping to `Makefile`:

```make
ABLATION_VARIANTS := baseline no_cpuid fixed_32m no_boot_params no_e820 \
                     no_cmdline no_protected_mode no_uart
ABLATION_CPPFLAGS_baseline :=
ABLATION_CPPFLAGS_no_cpuid := -DSVMM_ABLATE_CPUID
ABLATION_CPPFLAGS_fixed_32m := -DSVMM_ABLATE_DYNAMIC_MEMORY
ABLATION_CPPFLAGS_no_boot_params := -DSVMM_ABLATE_BOOT_PARAMS
ABLATION_CPPFLAGS_no_e820 := -DSVMM_ABLATE_E820
ABLATION_CPPFLAGS_no_cmdline := -DSVMM_ABLATE_CMDLINE
ABLATION_CPPFLAGS_no_protected_mode := -DSVMM_ABLATE_PROTECTED_MODE
ABLATION_CPPFLAGS_no_uart := -DSVMM_ABLATE_UART

.PHONY: ablation
ablation: $(ABLATION_VARIANTS:%=bin/ablation/%/linux_boot)
```

Use a `define`/`eval` template so every variant compiles `$(SOURCES)` with:

```make
$(CPPFLAGS) $(CFLAGS) $(ABLATION_CPPFLAGS_<variant>) \
-DSVMM_VARIANT_NAME=\"<variant>\" -MMD -MP
```

and writes objects under `build/ablation/<variant>/`. Include all generated `.d` files with the existing dependency files.

- [ ] **Step 5: Run build isolation and default build tests**

Run:

```sh
sh tests/test_ablation_build.sh
make clean
make all
make test
```

Expected: all eight binaries exist, the double-ablation compile fails as intended inside the test, and the default test suite passes or reports only the existing readable-kernel KVM skip.

- [ ] **Step 6: Commit Task 1**

```sh
git add src/ablation.h tests/test_ablation_build.sh Makefile
git commit -m "Add isolated Stage 02 ablation builds"
```

---

### Task 2: Loader, memory, boot-parameter, e820, and command-line ablations

**Files:**
- Modify: `src/main.c`
- Modify: `src/memory.h`
- Modify: `src/boot/linux.c`
- Modify: `tests/test_boot.c`
- Create: `tests/test_loader_ablations.sh`
- Modify: `Makefile`

**Interfaces:**
- Consumes: the `SVMM_ABLATE_*_ENABLED` constants from Task 1.
- Produces: safe `fixed_32m`, `no_boot_params`, `no_e820`, and `no_cmdline` behavior observable in guest memory before KVM execution.

- [ ] **Step 1: Extend the boot fixture test with variant-specific assertions**

Include `ablation.h` in `tests/test_boot.c`. After `boot_linux_prepare()`, add compile-time expectations:

```c
#if SVMM_ABLATE_BOOT_PARAMS_ENABLED
    assert(params->hdr.header == 0);
#elif SVMM_ABLATE_E820_ENABLED
    assert(params->hdr.header == 0x53726448);
    assert(params->e820_entries == 0);
#elif SVMM_ABLATE_CMDLINE_ENABLED
    assert(params->hdr.cmd_line_ptr == 0);
    assert(memory.data[CMDLINE_ADDR] == 0);
#else
    assert(params->hdr.cmd_line_ptr == CMDLINE_ADDR);
    assert(params->e820_entries == 3);
#endif
```

For `SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED`, construct a fixture with `runtime_start + init_size > 32 MiB`, allocate exactly `32u * 1024u * 1024u`, and assert `boot_linux_prepare()` returns `-1` with `errno == EINVAL`. The main-program integration in Task 5 verifies that the `fixed_32m` binary selects this size.

- [ ] **Step 2: Add the loader variant test driver and run it red**

Create `tests/test_loader_ablations.sh` to compile and run `tests/test_boot.c` four times:

```sh
#!/bin/sh
set -eu
mkdir -p bin/ablation-tests
for item in \
    'fixed_32m:SVMM_ABLATE_DYNAMIC_MEMORY' \
    'no_boot_params:SVMM_ABLATE_BOOT_PARAMS' \
    'no_e820:SVMM_ABLATE_E820' \
    'no_cmdline:SVMM_ABLATE_CMDLINE'
do
    name=${item%%:*}
    macro=${item#*:}
    ${CC:-cc} -Isrc -std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE \
        -D"$macro" tests/test_boot.c src/boot/linux.c src/memory.c \
        -o "bin/ablation-tests/test_$name"
    "bin/ablation-tests/test_$name"
done
```

Run: `sh tests/test_loader_ablations.sh`

Expected: FAIL because the variant macros do not yet affect loader behavior.

- [ ] **Step 3: Implement the fixed 32 MiB memory variant safely**

Include `ablation.h` in `src/main.c` and `src/memory.h`. Define:

```c
#define BLOG_GUEST_MEMORY_SIZE (32u * 1024u * 1024u)
```

Select memory size in `main.c`:

```c
size_t memory_size = SVMM_ABLATE_DYNAMIC_MEMORY_ENABLED ?
    BLOG_GUEST_MEMORY_SIZE : boot_linux_memory_size(&image);
```

Adjust the lower-bound check in `guest_memory_init()` through a helper or conditional minimum so the experimental 32 MiB mapping can be registered with KVM. Keep the page-alignment check. Do not relax `boot_linux_prepare()`; a modern kernel whose initialization window exceeds 32 MiB must return `EINVAL` before any out-of-range copy.

- [ ] **Step 4: Implement the three boot-data ablations**

Include `ablation.h` in `src/boot/linux.c` and apply these exact rules:

```c
params.hdr.cmd_line_ptr = SVMM_ABLATE_CMDLINE_ENABLED ? 0 : CMDLINE_ADDR;
params.e820_entries = SVMM_ABLATE_E820_ENABLED ? 0 : 3;
```

Only populate e820 entries when `SVMM_ABLATE_E820_ENABLED == 0`. Only copy the command line when `SVMM_ABLATE_CMDLINE_ENABLED == 0`. Only copy the completed `struct boot_params` to `BOOT_PARAMS_ADDR` when `SVMM_ABLATE_BOOT_PARAMS_ENABLED == 0`; continue loading the kernel payload, setup copy, and GDT so this variant changes one component.

- [ ] **Step 5: Run loader ablation and baseline tests**

Run:

```sh
sh tests/test_loader_ablations.sh
make test
```

Expected: all four fixture variants pass; the baseline tests remain unchanged.

- [ ] **Step 6: Register the new test and commit Task 2**

Add `sh tests/test_loader_ablations.sh` to the `test` recipe before the KVM integration script.

```sh
git add src/main.c src/memory.h src/boot/linux.c tests/test_boot.c \
        tests/test_loader_ablations.sh Makefile
git commit -m "Add Stage 02 loader ablation points"
```

---

### Task 3: CPU, protected-mode, UART ablations, and vCPU statistics

**Files:**
- Modify: `src/vcpu.h`
- Modify: `src/vcpu.c`
- Modify: `src/main.c`
- Create: `tests/test_ablation_integration.sh`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `SVMM_ABLATE_CPUID_ENABLED`, `SVMM_ABLATE_PROTECTED_MODE_ENABLED`, and `SVMM_ABLATE_UART_ENABLED`.
- Produces: `struct vcpu_run_stats`, `int vcpu_run(struct vcpu *, struct serial *, struct vcpu_run_stats *)`, and functional `no_cpuid`, `no_protected_mode`, and `no_uart` binaries.

- [ ] **Step 1: Define statistics and make the integration test expect variant behavior**

Add to `src/vcpu.h`:

```c
struct vcpu_run_stats {
    unsigned long exits;
    unsigned long serial_exits;
    unsigned exit_reason;
    int entered;
};

int vcpu_run(struct vcpu *vcpu, struct serial *serial,
             struct vcpu_run_stats *stats);
```

Create `tests/test_ablation_integration.sh`:

```sh
#!/bin/sh
set -eu
kernel=${BZIMAGE_PATH:-/boot/vmlinuz-6.16.0}
[ -r "$kernel" ] || { echo "missing kernel: $kernel" >&2; exit 1; }
[ -r /dev/kvm ] && [ -w /dev/kvm ] || { echo '/dev/kvm unavailable' >&2; exit 1; }
make ablation

run_variant() {
    variant=$1
    out=$(mktemp)
    err=$(mktemp)
    if timeout 15 "bin/ablation/$variant/linux_boot" "$kernel" >"$out" 2>"$err"; then
        status=0
    else
        status=$?
    fi
    case "$variant" in
        baseline) grep -q 'Linux version' "$out" ;;
        no_uart) ! grep -q 'Linux version' "$out" ;;
        no_cpuid|no_protected_mode)
            [ "$status" -ne 0 ] || ! grep -q 'Linux version' "$out"
            ;;
    esac
    rm -f "$out" "$err"
}

run_variant baseline
run_variant no_cpuid
run_variant no_protected_mode
run_variant no_uart
```

- [ ] **Step 2: Compile to expose the signature mismatch**

Run: `make clean && make all`

Expected: FAIL after changing the header because `main.c` and `vcpu.c` still use the old `vcpu_run` signature.

- [ ] **Step 3: Populate vCPU statistics without changing baseline control flow**

At the start of `vcpu_run()` zero the caller-provided structure, set `entered = 1` after the first successful `KVM_RUN`, increment `exits` and `serial_exits` alongside existing counters, and set `exit_reason` before returning on HLT, shutdown, or unexpected exit. Replace local counters with fields from `stats` so one source of truth remains.

Update `main.c`:

```c
struct vcpu_run_stats run_stats = { 0 };
int run_result = vcpu_run(&vcpu, &serial, &run_stats);
```

- [ ] **Step 4: Implement CPUID, protected-mode, and UART compile-time removals**

In `vcpu_init()`, return success after mapping `kvm_run` when `SVMM_ABLATE_CPUID_ENABLED` is true; otherwise call `vcpu_setup_cpuid()`.

In `vcpu_setup_linux_boot()`, skip `KVM_GET_SREGS`, segment/GDT setup, CR0 changes, and `KVM_SET_SREGS` when `SVMM_ABLATE_PROTECTED_MODE_ENABLED` is true. Still set general registers and enter at the same compressed-kernel address so only CPU mode setup is removed.

In the `KVM_EXIT_IO` branch, treat COM1 as an unmodeled port when `SVMM_ABLATE_UART_ENABLED` is true. Reads return `0xff`, writes are discarded, and `serial_exits` remains zero.

- [ ] **Step 5: Run the actual-KVM integration test and default suite**

Run:

```sh
BZIMAGE_PATH=/boot/vmlinuz-6.16.0 sh tests/test_ablation_integration.sh
make test
```

Expected: baseline prints `Linux version`; `no_uart` has no captured Linux serial log; CPUID and protected-mode variants do not produce a false successful Linux boot; default tests pass.

- [ ] **Step 6: Register the optional strict integration test and commit Task 3**

Keep `tests/test_ablation_integration.sh` separate from plain `make test` because it requires KVM and a host kernel. Add a README note only in Task 6.

```sh
git add src/vcpu.h src/vcpu.c src/main.c tests/test_ablation_integration.sh Makefile
git commit -m "Add CPU and UART ablation points"
```

---

### Task 4: Stable machine-readable VMM metrics

**Files:**
- Create: `src/metrics.h`
- Create: `src/metrics.c`
- Create: `tests/test_metrics.c`
- Modify: `src/main.c`
- Modify: `src/vcpu.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `struct vcpu_run_stats` and `SVMM_VARIANT_NAME`.
- Produces: newline-delimited stderr records beginning with `SVMM_METRIC`, using fixed `key=value` tokens.

- [ ] **Step 1: Write the failing formatting test**

Define the public interface in the test:

```c
void metric_stage(FILE *stream, const char *variant, const char *stage,
                  size_t guest_memory_bytes);
void metric_exit(FILE *stream, const char *variant, const char *reason,
                 const struct vcpu_run_stats *stats);
```

Use `open_memstream()` in `tests/test_metrics.c`, call both functions, and assert exact strings:

```text
SVMM_METRIC variant=baseline stage=memory guest_memory_bytes=134217728
SVMM_METRIC variant=baseline stage=vcpu_exit reason=hlt entered=1 exits=42 serial_exits=7
```

- [ ] **Step 2: Run the test and confirm metrics are undefined**

Run:

```sh
${CC:-cc} -Isrc -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE \
    tests/test_metrics.c src/metrics.c -o bin/test_metrics
```

Expected: FAIL because `src/metrics.c` and `src/metrics.h` do not exist.

- [ ] **Step 3: Implement the focused formatter**

Implement `metric_stage()` and `metric_exit()` in `src/metrics.c` with one `fprintf()` per record. Keep keys in the tested order and represent booleans as `0` or `1`. Do not include free-form strings in metric values.

- [ ] **Step 4: Emit lifecycle and failure metrics**

In `main.c`, emit stages after image load, memory selection, successful memory mapping, boot preparation, vCPU setup, and before entering KVM. On every existing `goto done` failure, emit a fixed reason token such as `image_load`, `kvm_init`, `guest_memory`, `boot_prepare`, `vcpu_init`, or `vcpu_setup`.

In `vcpu.c`, emit exactly one exit record for HLT, shutdown, KVM error, and unexpected exits. Preserve existing human-readable diagnostics.

- [ ] **Step 5: Run formatter, baseline, and strict KVM checks**

Run:

```sh
make bin/test_metrics
./bin/test_metrics
make test
BZIMAGE_PATH=/boot/vmlinuz-6.16.0 REQUIRE_KERNEL_LOG=1 make test
```

Expected: formatter test passes, normal tests pass, and strict stderr contains `SVMM_METRIC` memory, KVM-entry, and vCPU-exit records.

- [ ] **Step 6: Add the metrics test to Make and commit Task 4**

Add `src/metrics.c` to `SOURCES`, add `bin/test_metrics`, and run it from `make test`.

```sh
git add src/metrics.h src/metrics.c tests/test_metrics.c src/main.c src/vcpu.c Makefile
git commit -m "Emit stable Stage 02 experiment metrics"
```

---

### Task 5: Experiment runner, classifier, aggregation, and report generator

**Files:**
- Create: `experiments/stage02/run.sh`
- Create: `experiments/stage02/collect.py`
- Create: `tests/test_ablation_collect.py`
- Modify: `.gitignore`
- Modify: `Makefile`

**Interfaces:**
- Consumes: isolated binaries from Task 1 and `SVMM_METRIC` records from Task 4.
- Produces: `raw.csv`, `summary.csv`, `report.md`, and per-run stdout/stderr logs under `experiments/stage02/results/`.

- [ ] **Step 1: Write classifier and CSV tests first**

Create `tests/test_ablation_collect.py` with `unittest` cases covering:

```python
class ClassificationTests(unittest.TestCase):
    def test_panic_takes_priority_over_timeout(self):
        result = classify(
            stdout="Linux version 6.18\nKernel panic - not syncing: VFS: Unable to mount root fs",
            stderr="SVMM_METRIC variant=baseline stage=kvm_run guest_memory_bytes=1\n",
            returncode=124,
            timed_out=True,
        )
        self.assertEqual(result.primary, "panic")
        self.assertTrue(result.timed_out)

    def test_shutdown_is_triple_fault(self):
        result = classify("", "SVMM_METRIC variant=x stage=vcpu_exit reason=shutdown entered=1 exits=1 serial_exits=0\n", 1, False)
        self.assertEqual(result.primary, "triple_fault")

    def test_csv_round_trip_with_special_path(self):
        path = "results/logs/kernel name,run 1.stdout"
        encoded = encode_csv_row({"stdout_path": path})
        self.assertEqual(decode_csv_row(encoded)["stdout_path"], path)
```

Add `test_prerequisites_preserve_results` using a temporary directory containing a sentinel `raw.csv`, call the prerequisite validator with a missing kernel, and assert the sentinel remains unchanged.

Add `test_timeout_kills_process_group` using a temporary shell child that spawns `sleep 60`; run it with a 50 ms timeout, then assert the recorded process group no longer exists.

- [ ] **Step 2: Run the collector tests red**

Run: `python3 -m unittest -v tests/test_ablation_collect.py`

Expected: FAIL because `experiments.stage02.collect` does not exist.

- [ ] **Step 3: Implement pure parsing and aggregation functions**

In `collect.py`, define typed records with `dataclasses` and these functions:

The exact public signatures are:

```text
parse_metrics(stderr: str) -> dict[str, str]
classify(stdout: str, stderr: str, returncode: int, timed_out: bool) -> RunResult
summarize(rows: list[dict[str, str]]) -> list[dict[str, str]]
encode_csv_row(row: dict[str, str]) -> str
decode_csv_row(text: str) -> dict[str, str]
```

Use `csv.DictWriter` and `csv.DictReader`; use `statistics.fmean`, `min`, and `max` only on present numeric values. Implement the classification priority exactly as the specification: loader rejection, triple fault, VFS panic, HLT, booted, timeout, error.

- [ ] **Step 4: Implement safe process execution and prerequisite validation**

Before creating a temporary results directory, verify both kernels are readable, `/dev/kvm` is readable and writable, `/usr/bin/time` exists, and every variant builds successfully. Raise a clear error before touching the current result directory if any check fails.

Start each VMM in a new process session. On timeout, send `SIGTERM` to the process group, wait briefly, then send `SIGKILL` if required. Capture `/usr/bin/time -f '%e %M'` into a separate temporary file. Return wall milliseconds, maximum RSS, return code, and `timed_out` independently.

- [ ] **Step 5: Implement the experiment schedule and atomic output replacement**

Use these constants unless CLI options override them:

```python
VARIANTS = (
    "baseline", "no_cpuid", "fixed_32m", "no_boot_params",
    "no_e820", "no_cmdline", "no_protected_mode", "no_uart",
)
PRIMARY_KERNEL = "/boot/vmlinuz-6.16.0"
COMPAT_KERNEL = "/boot/vmlinuz-6.18.0.bak"
TIMEOUT_SECONDS = 15
PERFORMANCE_RUNS = 10
```

Run one functional pass for every kernel/variant pair. Select performance candidates only when the primary functional row has `linux_version=1` and primary result `halted` or `panic`. Write logs and CSV to a sibling temporary directory, then rename it to `results` after every file and report is complete. Preserve the previous directory as `results.previous` only during the rename and delete it after success.

- [ ] **Step 6: Generate a deterministic Markdown report**

The report must contain environment metadata, kernel hashes, method, functional result table, performance table, per-variant interpretation, and limitations. Generate rows in declared variant order and use fixed decimal precision so repeated generation from identical CSV is byte-for-byte stable.

Create `run.sh` as a small wrapper:

```sh
#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec python3 "$repo_dir/experiments/stage02/collect.py" "$@"
```

- [ ] **Step 7: Run all collector unit tests**

Run:

```sh
python3 -m unittest -v tests/test_ablation_collect.py
make test
```

Expected: prerequisite preservation, timeout cleanup, classification, CSV round trip, aggregation, and existing C tests all pass.

- [ ] **Step 8: Register tests and ignore transient files, then commit Task 5**

Ignore only collector temporary directories and `results.previous`; keep final CSV, report, and logs trackable. Add the Python test to `make test`.

```sh
git add experiments/stage02/run.sh experiments/stage02/collect.py \
        tests/test_ablation_collect.py .gitignore Makefile
git commit -m "Add Stage 02 ablation experiment runner"
```

---

### Task 6: Execute the full experiment and publish results

**Files:**
- Create: `experiments/stage02/results/raw.csv`
- Create: `experiments/stage02/results/summary.csv`
- Create: `experiments/stage02/results/report.md`
- Create: `experiments/stage02/results/logs/*`
- Modify: `README.md`

**Interfaces:**
- Consumes: the complete experiment runner and both default host kernels.
- Produces: committed evidence and reproduction instructions for the Stage 02 ablation conclusions.

- [ ] **Step 1: Run clean baseline verification before experiments**

Run:

```sh
make clean
make all
make test
BZIMAGE_PATH=/boot/vmlinuz-6.16.0 REQUIRE_KERNEL_LOG=1 make test
BZIMAGE_PATH=/boot/vmlinuz-6.18.0.bak REQUIRE_KERNEL_LOG=1 make test
```

Expected: default build and tests pass; 6.16 reaches its known HLT endpoint and 6.18 reaches the accepted VFS panic boundary.

- [ ] **Step 2: Execute the complete experiment**

Run:

```sh
experiments/stage02/run.sh \
    --primary-kernel /boot/vmlinuz-6.16.0 \
    --compat-kernel /boot/vmlinuz-6.18.0.bak \
    --performance-runs 10 \
    --timeout 15
```

Expected: exit 0 after every functional variant and eligible performance repetition is recorded, including failures and timeouts.

- [ ] **Step 3: Validate generated evidence mechanically**

Add a `--validate-results` mode to `collect.py` if validation is not already a reusable function. It must check:

- exactly 16 functional rows exist: 8 variants × 2 kernels;
- each eligible performance variant has exactly 10 primary-kernel performance rows;
- every row names existing stdout and stderr logs;
- all CSV rows have the declared header width;
- summary counts equal grouped raw-row counts;
- report kernel hashes match raw metadata.

Run: `python3 experiments/stage02/collect.py --validate-results`

Expected: `PASS: Stage 02 ablation results are internally consistent`.

- [ ] **Step 4: Review the report against raw logs**

For each variant, open at least its primary functional stdout and stderr log and confirm the report classification matches the evidence. Check specifically that `fixed_32m` is rejected safely, shutdown is labeled as possible triple fault, and missing UART output is not described as proof that the guest stopped.

- [ ] **Step 5: Add reproduction documentation**

Append a README section with:

```sh
make ablation
experiments/stage02/run.sh
python3 experiments/stage02/collect.py --validate-results
```

Document the two default kernel paths, override flags, 15-second timeout, approximate runtime, output directory, and the distinction between functional outcomes and performance comparisons.

- [ ] **Step 6: Run final verification after result generation**

Run:

```sh
git diff --check
make clean
make all
make test
BZIMAGE_PATH=/boot/vmlinuz-6.16.0 REQUIRE_KERNEL_LOG=1 make test
python3 -m unittest -v tests/test_ablation_collect.py
python3 experiments/stage02/collect.py --validate-results
git status --short
```

Expected: every command exits 0; status contains only the intended README and experiment result artifacts.

- [ ] **Step 7: Commit results and documentation**

```sh
git add README.md experiments/stage02/results
git commit -m "Publish Stage 02 ablation experiment results"
```

- [ ] **Step 8: Request final whole-branch review and push**

Review the full diff from `490ccaa` through the result commit for critical correctness issues, resolve any findings, repeat the affected verification commands, then push `stage02`. Verify the remote branch head equals local `HEAD`.

#!/usr/bin/env python3
"""Build, run, and summarize the Stage 02 ablation experiment."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import os
import platform
import shutil
import signal
import statistics
import subprocess
import tempfile
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

VARIANTS = (
    "baseline",
    "no_cpuid",
    "fixed_32m",
    "no_boot_params",
    "no_e820",
    "no_cmdline",
    "no_protected_mode",
    "no_uart",
)
PRIMARY_KERNEL = "/boot/vmlinuz-6.16.0"
COMPAT_KERNEL = "/boot/vmlinuz-6.18.0.bak"
TIMEOUT_SECONDS = 15.0
PERFORMANCE_RUNS = 10
RESULT_KINDS = (
    "loader_rejected",
    "triple_fault",
    "panic",
    "halted",
    "booted",
    "timeout",
    "error",
)
RAW_FIELDS = (
    "kernel",
    "kernel_path",
    "kernel_sha256",
    "variant",
    "experiment",
    "run",
    "guest_memory_bytes",
    "entered_kvm",
    "returncode",
    "primary_result",
    "timed_out",
    "linux_version",
    "e820",
    "serial_output",
    "panic",
    "exit_reason",
    "exits",
    "serial_exits",
    "wall_ms",
    "max_rss_kib",
    "stdout_path",
    "stderr_path",
)
SUMMARY_FIELDS = (
    "kernel",
    "variant",
    "experiment",
    "runs",
    *RESULT_KINDS,
    "success_rate",
    "wall_ms_avg",
    "wall_ms_min",
    "wall_ms_max",
    "exits_avg",
    "exits_min",
    "exits_max",
    "serial_exits_avg",
    "serial_exits_min",
    "serial_exits_max",
    "max_rss_kib_avg",
    "max_rss_kib_min",
    "max_rss_kib_max",
)


@dataclass(frozen=True)
class RunResult:
    primary: str
    timed_out: bool
    linux_version: bool
    e820: bool
    serial_output: bool
    panic: bool
    entered: int
    guest_memory_bytes: int
    exit_reason: str
    exits: int
    serial_exits: int


@dataclass(frozen=True)
class Execution:
    returncode: int
    timed_out: bool
    wall_ms: int
    max_rss_kib: int


def parse_metrics(stderr: str) -> dict[str, str]:
    metrics: dict[str, str] = {}
    for line in stderr.splitlines():
        if not line.startswith("SVMM_METRIC "):
            continue
        for token in line.split()[1:]:
            if "=" in token:
                key, value = token.split("=", 1)
                metrics[key] = value
    return metrics


def _as_int(value: str | None) -> int:
    try:
        return int(value or "0")
    except ValueError:
        return 0


def classify(
    stdout: str, stderr: str, returncode: int, timed_out: bool
) -> RunResult:
    metrics = parse_metrics(stderr)
    stage = metrics.get("stage", "")
    reason = metrics.get("reason", "")
    panic = "Kernel panic - not syncing:" in stdout
    linux_version = "Linux version" in stdout
    e820 = "BIOS-e820" in stdout
    loader_stages = {
        "failed_image_load",
        "failed_guest_memory",
        "failed_boot_prepare",
    }

    if stage in loader_stages:
        primary = "loader_rejected"
    elif reason == "shutdown":
        primary = "triple_fault"
    elif panic:
        primary = "panic"
    elif reason == "hlt":
        primary = "halted"
    elif linux_version:
        primary = "booted"
    elif timed_out:
        primary = "timeout"
    else:
        primary = "error"

    return RunResult(
        primary=primary,
        timed_out=timed_out,
        linux_version=linux_version,
        e820=e820,
        serial_output=bool(stdout.strip()),
        panic=panic,
        entered=max(_as_int(metrics.get("entered")), int(stage == "kvm_run")),
        guest_memory_bytes=_as_int(metrics.get("guest_memory_bytes")),
        exit_reason=reason,
        exits=_as_int(metrics.get("exits")),
        serial_exits=_as_int(metrics.get("serial_exits")),
    )


def encode_csv_row(row: dict[str, str]) -> str:
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=list(row))
    writer.writeheader()
    writer.writerow(row)
    return stream.getvalue()


def decode_csv_row(text: str) -> dict[str, str]:
    row = next(csv.DictReader(io.StringIO(text, newline="")))
    return dict(row)


def _numeric_stats(rows: list[dict[str, str]], field: str) -> dict[str, str]:
    values = [int(row[field]) for row in rows if row.get(field, "") != ""]
    if not values:
        return {f"{field}_avg": "", f"{field}_min": "", f"{field}_max": ""}
    return {
        f"{field}_avg": f"{statistics.fmean(values):.3f}",
        f"{field}_min": str(min(values)),
        f"{field}_max": str(max(values)),
    }


def summarize(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    groups: dict[tuple[str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        groups[(row["kernel"], row["variant"], row["experiment"])].append(row)

    summaries: list[dict[str, str]] = []
    for key in sorted(
        groups,
        key=lambda item: (
            0 if item[0] == "primary" else 1,
            VARIANTS.index(item[1]),
            0 if item[2] == "functional" else 1,
        ),
    ):
        grouped = groups[key]
        counts = {
            kind: sum(row["primary_result"] == kind for row in grouped)
            for kind in RESULT_KINDS
        }
        successful = sum(
            row["primary_result"] in {"halted", "panic", "booted"}
            for row in grouped
        )
        summary = {
            "kernel": key[0],
            "variant": key[1],
            "experiment": key[2],
            "runs": str(len(grouped)),
            **{kind: str(counts[kind]) for kind in RESULT_KINDS},
            "success_rate": f"{successful / len(grouped):.3f}",
        }
        for field in ("wall_ms", "exits", "serial_exits", "max_rss_kib"):
            summary.update(_numeric_stats(grouped, field))
        summaries.append(summary)
    return summaries


def validate_prerequisites(
    repo: Path,
    primary_kernel: Path,
    compat_kernel: Path,
    results_dir: Path,
    *,
    require_kvm: bool = True,
    require_binaries: bool = True,
) -> None:
    del results_dir  # Validation must not inspect or mutate existing results.
    for kernel in (primary_kernel, compat_kernel):
        if not kernel.is_file() or not os.access(kernel, os.R_OK):
            raise RuntimeError(f"kernel is not readable: {kernel}")
    if require_kvm and not (
        os.access("/dev/kvm", os.R_OK) and os.access("/dev/kvm", os.W_OK)
    ):
        raise RuntimeError("/dev/kvm is not readable and writable")
    if not Path("/usr/bin/time").is_file():
        raise RuntimeError("/usr/bin/time is unavailable")
    if require_binaries:
        for variant in VARIANTS:
            binary = repo / "bin" / "ablation" / variant / "linux_boot"
            if not os.access(binary, os.X_OK):
                raise RuntimeError(f"ablation binary is unavailable: {binary}")


def _read_timing(path: Path) -> tuple[int, int]:
    try:
        fields = path.read_text().strip().split()
        return int(float(fields[-2]) * 1000), int(fields[-1])
    except (FileNotFoundError, IndexError, ValueError):
        return 0, 0


def _process_group_exists(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False


def _terminate_process_group(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return

    try:
        process.wait(timeout=0.2)
    except subprocess.TimeoutExpired:
        pass

    if _process_group_exists(process.pid):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    process.wait()

    deadline = time.monotonic() + 1.0
    while _process_group_exists(process.pid) and time.monotonic() < deadline:
        time.sleep(0.01)


def run_timed(
    command: list[str],
    timeout_seconds: float,
    stdout_path: Path,
    stderr_path: Path,
    timing_path: Path,
) -> Execution:
    timed_command = [
        "/usr/bin/time",
        "-f",
        "%e %M",
        "-o",
        str(timing_path),
        "--",
        *command,
    ]
    start = time.monotonic()
    timed_out = False
    with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
        process = subprocess.Popen(
            timed_command,
            stdout=stdout_file,
            stderr=stderr_file,
            start_new_session=True,
        )
        try:
            returncode = process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            _terminate_process_group(process)
            returncode = 124
        except BaseException:
            _terminate_process_group(process)
            raise
    measured_ms = int((time.monotonic() - start) * 1000)
    time_ms, max_rss_kib = _read_timing(timing_path)
    return Execution(
        returncode=returncode,
        timed_out=timed_out,
        wall_ms=time_ms or measured_ms,
        max_rss_kib=max_rss_kib,
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _write_csv(path: Path, fieldnames: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def _result_row(
    repo: Path,
    output_dir: Path,
    kernel_label: str,
    kernel_path: Path,
    kernel_sha256: str,
    variant: str,
    experiment: str,
    run: int,
    timeout_seconds: float,
) -> dict[str, str]:
    stem = f"{kernel_label}-{variant}-{experiment}-{run:02d}"
    logs = output_dir / "logs"
    stdout_path = logs / f"{stem}.stdout"
    stderr_path = logs / f"{stem}.stderr"
    timing_path = logs / f"{stem}.time"
    binary = repo / "bin" / "ablation" / variant / "linux_boot"
    execution = run_timed(
        [str(binary), str(kernel_path)],
        timeout_seconds,
        stdout_path,
        stderr_path,
        timing_path,
    )
    stdout = stdout_path.read_text(errors="replace")
    stderr = stderr_path.read_text(errors="replace")
    result = classify(stdout, stderr, execution.returncode, execution.timed_out)
    timing_path.unlink(missing_ok=True)
    final_prefix = Path("experiments/stage02/results/logs")
    has_final_exit = bool(result.exit_reason) or not result.entered
    return {
        "kernel": kernel_label,
        "kernel_path": str(kernel_path),
        "kernel_sha256": kernel_sha256,
        "variant": variant,
        "experiment": experiment,
        "run": str(run),
        "guest_memory_bytes": str(result.guest_memory_bytes),
        "entered_kvm": str(result.entered),
        "returncode": str(execution.returncode),
        "primary_result": result.primary,
        "timed_out": "1" if result.timed_out else "0",
        "linux_version": "1" if result.linux_version else "0",
        "e820": "1" if result.e820 else "0",
        "serial_output": "1" if result.serial_output else "0",
        "panic": "1" if result.panic else "0",
        "exit_reason": result.exit_reason,
        "exits": str(result.exits) if has_final_exit else "",
        "serial_exits": str(result.serial_exits) if has_final_exit else "",
        "wall_ms": str(execution.wall_ms),
        "max_rss_kib": str(execution.max_rss_kib) if execution.max_rss_kib else "",
        "stdout_path": str(final_prefix / stdout_path.name),
        "stderr_path": str(final_prefix / stderr_path.name),
    }


def _kernel_metadata(path: Path) -> dict[str, str]:
    return {
        "path": str(path),
        "size": str(path.stat().st_size),
        "sha256": _sha256(path),
    }


def _report(
    path: Path,
    rows: list[dict[str, str]],
    summaries: list[dict[str, str]],
    kernels: dict[str, dict[str, str]],
    performance_runs: int,
    timeout_seconds: float,
) -> None:
    functional = [row for row in rows if row["experiment"] == "functional"]
    performance = [row for row in summaries if row["experiment"] == "performance"]
    timed_out_runs: dict[tuple[str, str], int] = defaultdict(int)
    for row in rows:
        if row["experiment"] == "performance" and row["timed_out"] == "1":
            timed_out_runs[(row["kernel"], row["variant"])] += 1
    lines = [
        "# Stage 02 消融实验报告",
        "",
        "## 环境",
        "",
        f"- 平台：`{platform.platform()}`",
        f"- Python：`{platform.python_version()}`",
    ]
    for label in ("primary", "compat"):
        metadata = kernels[label]
        lines.append(
            f"- {label} 内核：`{metadata['path']}`，{metadata['size']} bytes，"
            f"SHA-256 `{metadata['sha256']}`"
        )
    lines.extend(
        [
            "",
            "## 方法",
            "",
            f"每个变体和内核执行一次功能实验，单次超时 {timeout_seconds:g} 秒。"
            f"主内核中达到可识别内核终点的变体额外执行 {performance_runs} 次性能实验。",
            "每个二进制只启用一个编译期消融宏，构建目录相互隔离。",
            "",
            "## 功能结果",
            "",
            "| 内核 | 变体 | 结果 | Linux 日志 | e820 | 串口输出 | KVM exits | 超时 |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for row in functional:
        lines.append(
            f"| {row['kernel']} | {row['variant']} | {row['primary_result']} | "
            f"{row['linux_version']} | {row['e820']} | {row['serial_output']} | "
            f"{row['exits'] or '未记录'} | {row['timed_out']} |"
        )
    lines.extend(
        [
            "",
            "## 性能结果",
            "",
            "| 变体 | 次数 | 终点识别率 | 超时次数 | 时间均值 ms | 时间范围 ms | exits 均值 | RSS 均值 KiB |",
            "| --- | ---: | ---: | ---: | ---: | --- | ---: | ---: |",
        ]
    )
    if performance:
        for row in performance:
            lines.append(
                f"| {row['variant']} | {row['runs']} | {row['success_rate']} | "
                f"{timed_out_runs[(row['kernel'], row['variant'])]} | "
                f"{row['wall_ms_avg']} | "
                f"{row['wall_ms_min']}–{row['wall_ms_max']} | "
                f"{row['exits_avg']} | {row['max_rss_kib_avg']} |"
            )
    else:
        lines.append("| 无符合条件的变体 | 0 | 0.000 | 0 |  |  |  |  |")
    lines.extend(["", "## 逐项解释", ""])
    primary_rows = {row["variant"]: row for row in functional if row["kernel"] == "primary"}
    for variant in VARIANTS:
        row = primary_rows[variant]
        note = f"结果为 `{row['primary_result']}`"
        if row["linux_version"] == "0":
            note += "，没有串口证据证明内核到达 Linux version"
        if variant in {"no_uart", "no_cmdline"} and row["serial_output"] == "0":
            note += "；缺少串口输出不等同于客户机停止"
        lines.append(f"- `{variant}`：{note}。")
    lines.extend(
        [
            "",
            "## 限制",
            "",
            "- 结果只适用于记录的内核、宿主机 KVM 和当前 Stage 02 实现。",
            "- 超时表示观察窗口内没有终止，不能单独证明客户机崩溃。",
            "- 性能表中的超时运行墙钟值是观察窗口长度，不代表到达 panic 或成功启动的耗时。",
            "- 无 Linux 串口日志的变体只能依据宿主机 KVM exit 判断进度。",
            "- 超时终止的进程无法输出最终 exit 计数，且 `/usr/bin/time` 可能无法写入 RSS；这些不可得值留空。",
            "- 最大 RSS 包含 VMM 进程及 `/usr/bin/time` 观测到的宿主机开销。",
            "",
        ]
    )
    path.write_text("\n".join(lines))


def _replace_results(temp_results: Path, results: Path) -> None:
    previous = results.with_name("results.previous")
    if previous.exists():
        shutil.rmtree(previous)
    try:
        if results.exists():
            results.rename(previous)
        temp_results.rename(results)
    except BaseException:
        if not results.exists() and previous.exists():
            previous.rename(results)
        raise
    if previous.exists():
        shutil.rmtree(previous)


def run_experiment(
    repo: Path,
    primary_kernel: Path,
    compat_kernel: Path,
    performance_runs: int,
    timeout_seconds: float,
) -> None:
    experiment_dir = repo / "experiments" / "stage02"
    results = experiment_dir / "results"
    validate_prerequisites(
        repo,
        primary_kernel,
        compat_kernel,
        results,
        require_binaries=False,
    )
    subprocess.run(["make", "ablation"], cwd=repo, check=True)
    validate_prerequisites(repo, primary_kernel, compat_kernel, results)

    temp_results = Path(tempfile.mkdtemp(prefix="results.tmp.", dir=experiment_dir))
    (temp_results / "logs").mkdir()
    kernels = {
        "primary": _kernel_metadata(primary_kernel),
        "compat": _kernel_metadata(compat_kernel),
    }
    rows: list[dict[str, str]] = []
    try:
        for label, kernel in (("primary", primary_kernel), ("compat", compat_kernel)):
            for variant in VARIANTS:
                print(f"functional {label} {variant}", flush=True)
                rows.append(
                    _result_row(
                        repo,
                        temp_results,
                        label,
                        kernel,
                        kernels[label]["sha256"],
                        variant,
                        "functional",
                        1,
                        timeout_seconds,
                    )
                )
        eligible = [
            row["variant"]
            for row in rows
            if row["kernel"] == "primary"
            and row["linux_version"] == "1"
            and row["primary_result"] in {"halted", "panic"}
        ]
        for variant in eligible:
            for run in range(1, performance_runs + 1):
                print(f"performance primary {variant} {run}/{performance_runs}", flush=True)
                rows.append(
                    _result_row(
                        repo,
                        temp_results,
                        "primary",
                        primary_kernel,
                        kernels["primary"]["sha256"],
                        variant,
                        "performance",
                        run,
                        timeout_seconds,
                    )
                )
        summaries = summarize(rows)
        _write_csv(temp_results / "raw.csv", RAW_FIELDS, rows)
        _write_csv(temp_results / "summary.csv", SUMMARY_FIELDS, summaries)
        _report(
            temp_results / "report.md",
            rows,
            summaries,
            kernels,
            performance_runs,
            timeout_seconds,
        )
        _replace_results(temp_results, results)
    except BaseException:
        shutil.rmtree(temp_results, ignore_errors=True)
        raise


def validate_results(repo: Path, results: Path, performance_runs: int) -> None:
    raw_path = results / "raw.csv"
    summary_path = results / "summary.csv"
    report_path = results / "report.md"
    try:
        with raw_path.open(newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != list(RAW_FIELDS):
                raise RuntimeError("raw.csv header does not match the declared schema")
            rows = list(reader)
        with summary_path.open(newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != list(SUMMARY_FIELDS):
                raise RuntimeError("summary.csv header does not match the declared schema")
            summary_rows = list(reader)
        report = report_path.read_text()
    except FileNotFoundError as error:
        raise RuntimeError(f"result artifact is missing: {error.filename}") from error

    all_csv_rows = [*rows, *summary_rows]
    if any(None in row for row in all_csv_rows):
        raise RuntimeError("CSV row has more columns than its declared header")
    if any(any(value is None for value in row.values()) for row in all_csv_rows):
        raise RuntimeError("CSV row has missing columns")

    raw_integer_fields = (
        "run",
        "guest_memory_bytes",
        "entered_kvm",
        "returncode",
        "timed_out",
        "linux_version",
        "e820",
        "serial_output",
        "panic",
        "exits",
        "serial_exits",
        "wall_ms",
        "max_rss_kib",
    )
    summary_integer_fields = (
        "runs",
        *RESULT_KINDS,
        "wall_ms_min",
        "wall_ms_max",
        "exits_min",
        "exits_max",
        "serial_exits_min",
        "serial_exits_max",
        "max_rss_kib_min",
        "max_rss_kib_max",
    )
    summary_float_fields = (
        "success_rate",
        "wall_ms_avg",
        "exits_avg",
        "serial_exits_avg",
        "max_rss_kib_avg",
    )
    for label, csv_rows, fields, conversion in (
        ("raw", rows, raw_integer_fields, int),
        ("summary", summary_rows, summary_integer_fields, int),
        ("summary", summary_rows, summary_float_fields, float),
    ):
        for line, row in enumerate(csv_rows, start=2):
            for field in fields:
                value = row[field]
                if value == "":
                    continue
                try:
                    conversion(value)
                except ValueError as error:
                    raise RuntimeError(
                        f"{label}.csv line {line} has non-numeric {field}: {value}"
                    ) from error

    functional = [row for row in rows if row["experiment"] == "functional"]
    expected_matrix = {(kernel, variant) for kernel in ("primary", "compat") for variant in VARIANTS}
    actual_matrix = {(row["kernel"], row["variant"]) for row in functional}
    if len(functional) != 16 or actual_matrix != expected_matrix:
        raise RuntimeError("expected exactly 16 functional rows covering both kernels")

    eligible = {
        row["variant"]
        for row in functional
        if row["kernel"] == "primary"
        and row["linux_version"] == "1"
        and row["primary_result"] in {"halted", "panic"}
    }
    performance = [row for row in rows if row["experiment"] == "performance"]
    for variant in VARIANTS:
        variant_rows = [
            row
            for row in performance
            if row["kernel"] == "primary" and row["variant"] == variant
        ]
        expected = performance_runs if variant in eligible else 0
        if len(variant_rows) != expected:
            raise RuntimeError(
                f"expected {expected} performance rows for {variant}, got {len(variant_rows)}"
            )
    if any(row["kernel"] != "primary" for row in performance):
        raise RuntimeError("performance rows must use the primary kernel")

    for row in rows:
        for field in ("stdout_path", "stderr_path"):
            if not (repo / row[field]).is_file():
                raise RuntimeError(f"referenced log is missing: {row[field]}")

    expected_summaries = {
        (row["kernel"], row["variant"], row["experiment"]): row
        for row in summarize(rows)
    }
    summary_keys = [
        (row["kernel"], row["variant"], row["experiment"])
        for row in summary_rows
    ]
    if len(summary_keys) != len(set(summary_keys)):
        raise RuntimeError("duplicate summary group")
    actual_summaries = dict(zip(summary_keys, summary_rows))
    if set(actual_summaries) != set(expected_summaries):
        raise RuntimeError("summary groups do not match raw.csv groups")
    for key, expected in expected_summaries.items():
        actual = actual_summaries[key]
        for field in SUMMARY_FIELDS:
            if actual[field] != expected[field]:
                raise RuntimeError(f"summary mismatch for {key}: {field}")

    for digest in {row["kernel_sha256"] for row in rows}:
        if digest not in report:
            raise RuntimeError(f"report does not contain kernel hash: {digest}")


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--primary-kernel", type=Path, default=Path(PRIMARY_KERNEL))
    parser.add_argument("--compat-kernel", type=Path, default=Path(COMPAT_KERNEL))
    parser.add_argument("--performance-runs", type=int, default=PERFORMANCE_RUNS)
    parser.add_argument("--timeout", type=float, default=TIMEOUT_SECONDS)
    parser.add_argument("--validate-results", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = _arguments()
    if args.performance_runs < 1:
        raise SystemExit("--performance-runs must be at least 1")
    if args.timeout <= 0:
        raise SystemExit("--timeout must be positive")
    repo = Path(__file__).resolve().parents[2]
    if args.validate_results:
        validate_results(
            repo,
            repo / "experiments" / "stage02" / "results",
            args.performance_runs,
        )
        print("PASS: Stage 02 ablation results are internally consistent")
        return 0
    run_experiment(
        repo,
        args.primary_kernel.resolve(),
        args.compat_kernel.resolve(),
        args.performance_runs,
        args.timeout,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

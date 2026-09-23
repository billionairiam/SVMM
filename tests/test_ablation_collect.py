import csv
import os
import signal
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from experiments.stage02.collect import (
    Execution,
    RAW_FIELDS,
    SUMMARY_FIELDS,
    _result_row,
    _write_csv,
    classify,
    decode_csv_row,
    encode_csv_row,
    run_timed,
    summarize,
    validate_prerequisites,
    validate_results,
)


class ClassificationTests(unittest.TestCase):
    def test_panic_takes_priority_over_timeout(self):
        result = classify(
            stdout=(
                "Linux version 6.18\n"
                "Kernel panic - not syncing: VFS: Unable to mount root fs"
            ),
            stderr=(
                "SVMM_METRIC variant=baseline stage=kvm_run "
                "guest_memory_bytes=1\n"
            ),
            returncode=124,
            timed_out=True,
        )
        self.assertEqual(result.primary, "panic")
        self.assertTrue(result.timed_out)
        self.assertTrue(result.linux_version)

    def test_any_kernel_panic_takes_priority_over_boot_progress(self):
        result = classify(
            stdout=(
                "Linux version 6.16\n"
                "Kernel panic - not syncing: alloc_low_pages: can not alloc memory"
            ),
            stderr=(
                "SVMM_METRIC variant=no_e820 stage=kvm_run "
                "guest_memory_bytes=134217728\n"
            ),
            returncode=124,
            timed_out=True,
        )
        self.assertEqual(result.primary, "panic")
        self.assertTrue(result.panic)

    def test_kvm_run_timeout_is_recorded_as_entered(self):
        result = classify(
            "",
            "SVMM_METRIC variant=no_uart stage=kvm_run "
            "guest_memory_bytes=134217728\n",
            124,
            True,
        )
        self.assertEqual(result.primary, "timeout")
        self.assertEqual(result.entered, 1)

    def test_shutdown_is_triple_fault(self):
        result = classify(
            "",
            "SVMM_METRIC variant=x stage=vcpu_exit reason=shutdown "
            "entered=1 exits=1 serial_exits=0\n",
            1,
            False,
        )
        self.assertEqual(result.primary, "triple_fault")
        self.assertEqual(result.exits, 1)

    def test_boot_prepare_failure_is_loader_rejection(self):
        result = classify(
            "",
            "SVMM_METRIC variant=fixed_32m stage=failed_boot_prepare "
            "guest_memory_bytes=33554432\n",
            1,
            False,
        )
        self.assertEqual(result.primary, "loader_rejected")
        self.assertEqual(result.guest_memory_bytes, 33554432)


class CsvAndSummaryTests(unittest.TestCase):
    def test_csv_round_trip_with_special_path(self):
        path = "results/logs/kernel name,run 1.stdout"
        encoded = encode_csv_row({"stdout_path": path})
        self.assertEqual(decode_csv_row(encoded)["stdout_path"], path)

    def test_result_csv_uses_repository_lf_line_endings(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "result.csv"
            _write_csv(path, ("value",), [{"value": "one"}])
            self.assertEqual(path.read_bytes(), b"value\none\n")

    def test_summary_uses_literal_group_counts_and_numeric_ranges(self):
        rows = [
            {
                "kernel": "primary",
                "variant": "baseline",
                "experiment": "performance",
                "primary_result": "halted",
                "wall_ms": "10",
                "exits": "40",
                "serial_exits": "5",
                "max_rss_kib": "100",
            },
            {
                "kernel": "primary",
                "variant": "baseline",
                "experiment": "performance",
                "primary_result": "halted",
                "wall_ms": "20",
                "exits": "44",
                "serial_exits": "7",
                "max_rss_kib": "120",
            },
        ]
        summary = summarize(rows)
        self.assertEqual(len(summary), 1)
        self.assertEqual(summary[0]["runs"], "2")
        self.assertEqual(summary[0]["halted"], "2")
        self.assertEqual(summary[0]["wall_ms_avg"], "15.000")
        self.assertEqual(summary[0]["exits_min"], "40")
        self.assertEqual(summary[0]["max_rss_kib_max"], "120")

    def test_timeout_keeps_unavailable_final_metrics_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            output = repo / "results"
            (output / "logs").mkdir(parents=True)

            def fake_run(command, timeout, stdout_path, stderr_path, timing_path):
                del command, timeout, timing_path
                stdout_path.write_text("Linux version 6.16\n")
                stderr_path.write_text(
                    "SVMM_METRIC variant=no_e820 stage=kvm_run "
                    "guest_memory_bytes=134217728\n"
                )
                return Execution(124, True, 15002, 0)

            with patch("experiments.stage02.collect.run_timed", fake_run):
                row = _result_row(
                    repo, output, "primary", Path("/boot/kernel"), "abc",
                    "no_e820", "functional", 1, 15,
                )
            self.assertEqual(row["entered_kvm"], "1")
            self.assertEqual(row["exits"], "")
            self.assertEqual(row["serial_exits"], "")
            self.assertEqual(row["max_rss_kib"], "")


class ProcessSafetyTests(unittest.TestCase):
    def test_prerequisites_preserve_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results = root / "results"
            results.mkdir()
            raw = results / "raw.csv"
            raw.write_text("sentinel\n")
            with self.assertRaisesRegex(RuntimeError, "kernel"):
                validate_prerequisites(
                    root,
                    root / "missing-primary",
                    root / "missing-compat",
                    results,
                    require_kvm=False,
                    require_binaries=False,
                )
            self.assertEqual(raw.read_text(), "sentinel\n")

    def test_timeout_kills_process_group(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pgid_file = root / "pgid"
            stdout_path = root / "stdout"
            stderr_path = root / "stderr"
            timing_path = root / "timing"
            command = [
                "/bin/sh",
                "-c",
                f"ps -o pgid= -p $$ > '{pgid_file}'; sleep 60 & wait",
            ]
            result = run_timed(
                command, 0.05, stdout_path, stderr_path, timing_path
            )
            self.assertTrue(result.timed_out)
            pgid = int(pgid_file.read_text().strip())
            with self.assertRaises(ProcessLookupError):
                os.killpg(pgid, 0)


class ResultValidationTests(unittest.TestCase):
    def _write_valid_results(self, repo: Path) -> Path:
        variants = (
            "baseline", "no_cpuid", "fixed_32m", "no_boot_params",
            "no_e820", "no_cmdline", "no_protected_mode", "no_uart",
        )
        results = repo / "experiments" / "stage02" / "results"
        logs = results / "logs"
        logs.mkdir(parents=True)
        raw_rows = []
        summary_rows = []
        for kernel, digest in (("primary", "aaa111"), ("compat", "bbb222")):
            for variant in variants:
                stem = f"{kernel}-{variant}"
                stdout_rel = f"experiments/stage02/results/logs/{stem}.stdout"
                stderr_rel = f"experiments/stage02/results/logs/{stem}.stderr"
                (repo / stdout_rel).write_text("")
                (repo / stderr_rel).write_text("")
                raw_rows.append({
                    "kernel": kernel,
                    "kernel_path": f"/boot/{kernel}",
                    "kernel_sha256": digest,
                    "variant": variant,
                    "experiment": "functional",
                    "run": "1",
                    "guest_memory_bytes": "0",
                    "entered_kvm": "0",
                    "returncode": "1",
                    "primary_result": "error",
                    "timed_out": "0",
                    "linux_version": "0",
                    "e820": "0",
                    "serial_output": "0",
                    "panic": "0",
                    "exit_reason": "",
                    "exits": "0",
                    "serial_exits": "0",
                    "wall_ms": "1",
                    "max_rss_kib": "1",
                    "stdout_path": stdout_rel,
                    "stderr_path": stderr_rel,
                })
                summary = {name: "" for name in SUMMARY_FIELDS}
                summary.update({
                    "kernel": kernel,
                    "variant": variant,
                    "experiment": "functional",
                    "runs": "1",
                    "loader_rejected": "0",
                    "triple_fault": "0",
                    "panic": "0",
                    "halted": "0",
                    "booted": "0",
                    "timeout": "0",
                    "error": "1",
                    "success_rate": "0.000",
                })
                summary_rows.append(summary)
        with (results / "raw.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=RAW_FIELDS)
            writer.writeheader()
            writer.writerows(raw_rows)
        with (results / "summary.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=SUMMARY_FIELDS)
            writer.writeheader()
            writer.writerows(summary_rows)
        (results / "report.md").write_text("hash aaa111 and hash bbb222\n")
        return results

    def test_validates_complete_functional_matrix(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            results = self._write_valid_results(repo)
            validate_results(repo, results, performance_runs=10)

    def test_rejects_missing_functional_row(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            results = self._write_valid_results(repo)
            with (results / "raw.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            with (results / "raw.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=RAW_FIELDS)
                writer.writeheader()
                writer.writerows(rows[:-1])
            with self.assertRaisesRegex(RuntimeError, "16 functional"):
                validate_results(repo, results, performance_runs=10)


if __name__ == "__main__":
    unittest.main()

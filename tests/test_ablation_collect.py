import os
import signal
import tempfile
import unittest
from pathlib import Path

from experiments.stage02.collect import (
    classify,
    decode_csv_row,
    encode_csv_row,
    run_timed,
    summarize,
    validate_prerequisites,
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


if __name__ == "__main__":
    unittest.main()

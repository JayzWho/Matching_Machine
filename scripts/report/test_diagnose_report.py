#!/usr/bin/env python3
"""Synthetic checks of launch grouping and layout metadata joins."""
import csv
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

import diagnose_report


class SummaryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mm-summary-test-")
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name)
        (self.path / "env.json").write_text(json.dumps({"complete": True, "design": {"repetitions": 5}}))
        self.samples = []
        self.launches = []
        for block, median in ((1, 100), (2, 200)):
            key = {"session": 1, "block": block, "condition": "named_condition", "binary": "test_binary"}
            self.launches.append({**key, "exit_status": 0, "wall_s": 2, "foreign_ms": block * 2,
                                  "throttle_core": 0, "throttle_package": 0, "device_irqs": 0,
                                  "irq_TRM": 0, "suspended_s": 0})
            for repetition, value in enumerate((median - 10, median, median, median, median + 10)):
                self.samples.append({**key, "benchmark": "BM_Test", "repetition": repetition,
                                     "real_time_ns": value})
        self.write_rows()

    def write_rows(self):
        for name, rows in (("samples", self.samples), ("launches", self.launches)):
            with (self.path / f"{name}.csv").open("w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=rows[0])
                writer.writeheader()
                writer.writerows(rows)

    def test_independent_launch_medians(self):
        rows = diagnose_report.summarize(self.path)
        result = next(row for row in rows if row["kind"] == "benchmark")
        self.assertEqual(result["launches"], 2)
        self.assertEqual(result["median_ns"], 150)
        self.assertEqual(result["spread_pct"], 100)
        self.assertAlmostEqual(result["iqr_pct"], 100 / 3)
        self.assertAlmostEqual(result["launch_cv_pct"], 47.14045207910317)
        telemetry = next(row for row in rows if row["kind"] == "telemetry")
        self.assertEqual(telemetry["foreign_ms_per_s_median"], 1.5)

    def test_layout_deduplicates_and_uses_metadata(self):
        raw = self.path / "raw"
        raw.mkdir()
        for block, index in ((1, 8), (2, 2)):
            stem = f"s1_b{block}_c{index}"
            (raw / f"{stem}.command.json").write_text(json.dumps({
                "complete": True, "condition": "named_condition"}))
            (raw / f"{stem}.log").write_text("MM_LAYOUT address=0x1010 mod64=16 mod4096=16\n" * 15)
        def layout():
            return next(row for row in diagnose_report.summarize(self.path) if row["kind"] == "layout")
        result = layout()
        self.assertEqual(result["condition"], "named_condition")
        self.assertEqual(result["layout_launches"], 2)
        self.assertEqual(result["unique_addresses"], 1)
        self.assertEqual(result["unique_mod64"], 1)
        self.assertEqual(result["unique_mod4096"], 1)
        self.assertEqual(result["multi_mod64_launches"], 0)
        with tarfile.open(self.path / "raw.tar.gz", "w:gz") as archive:
            archive.add(raw, arcname="raw")
        for path in raw.iterdir():
            path.unlink()
        raw.rmdir()
        self.assertEqual(layout(), result)

    def test_varying_layout_listed_by_launch(self):
        raw = self.path / "raw"
        raw.mkdir()
        for block, address in ((1, 0x1030), (2, 0x1000)):
            stem = f"s1_b{block}_c0"
            (raw / f"{stem}.command.json").write_text(json.dumps({
                "complete": True, "condition": "named_condition"}))
            (raw / f"{stem}.log").write_text(
                f"MM_LAYOUT address={address:#x} mod64={address % 64} mod4096={address % 4096}\n")
        rows = [row for row in diagnose_report.summarize(self.path) if row["kind"] == "launch_layout"]
        self.assertEqual([(row["block"], row["mod64"], row["BM_Test (ms)"]) for row in rows],
                         [(2, "0", 200 / 1e6), (1, "48", 100 / 1e6)])
        self.assertIn("### Layout by launch", diagnose_report.markdown(diagnose_report.summarize(self.path)))

    def test_missing_or_duplicate_repetition_rejected(self):
        removed = self.samples.pop()
        self.write_rows()
        with self.assertRaisesRegex(ValueError, "incomplete repetitions"):
            diagnose_report.summarize(self.path)
        self.samples.append(removed)
        self.samples.append(removed)
        self.write_rows()
        with self.assertRaisesRegex(ValueError, "duplicate repetition"):
            diagnose_report.summarize(self.path)

    def test_warmup_repetition_recorded_but_not_counted(self):
        (self.path / "env.json").write_text(json.dumps(
            {"complete": True, "design": {"repetitions": 6, "warmup_repetitions": 1}}))
        for row in self.samples:
            row["repetition"] += 1
            row["warmup"] = 0
        for launch in self.launches:
            key = {k: launch[k] for k in ("session", "block", "condition", "binary")}
            self.samples.append({**key, "benchmark": "BM_Test", "repetition": 0, "warmup": 1,
                                 "real_time_ns": 10**9})
        self.write_rows()
        result = next(row for row in diagnose_report.summarize(self.path) if row["kind"] == "benchmark")
        self.assertEqual(result["repetitions"], 5)
        self.assertEqual(result["median_ns"], 150)
        self.assertEqual(result["max_ns"], 200)
        # A counted repetition cannot be replaced by the warm-up.
        self.samples = [row for row in self.samples if row["repetition"] != 1]
        self.write_rows()
        with self.assertRaisesRegex(ValueError, "incomplete repetitions"):
            diagnose_report.summarize(self.path)

    def test_incomplete_study_rejected(self):
        (self.path / "env.json").write_text('{"complete": false}')
        with self.assertRaisesRegex(ValueError, "study is incomplete"):
            diagnose_report.summarize(self.path)


if __name__ == "__main__":
    unittest.main()

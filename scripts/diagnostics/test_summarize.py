#!/usr/bin/env python3
"""Synthetic checks of launch grouping and layout metadata joins."""
import csv
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

import summarize


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
        rows = summarize.summarize(self.path)
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
            return next(row for row in summarize.summarize(self.path) if row["kind"] == "layout")
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

    def test_missing_or_duplicate_repetition_rejected(self):
        removed = self.samples.pop()
        self.write_rows()
        with self.assertRaisesRegex(ValueError, "incomplete repetitions"):
            summarize.summarize(self.path)
        self.samples.append(removed)
        self.samples.append(removed)
        self.write_rows()
        with self.assertRaisesRegex(ValueError, "duplicate repetition"):
            summarize.summarize(self.path)

    def test_incomplete_study_rejected(self):
        (self.path / "env.json").write_text('{"complete": false}')
        with self.assertRaisesRegex(ValueError, "study is incomplete"):
            summarize.summarize(self.path)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Check diagnostic validation and failure cleanup without measurement mode."""
import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import bench_diagnose as diagnose
import bench_record as recorder


class DiagnosticTests(unittest.TestCase):
    def test_matrix_rejects_duplicates_and_reserved_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "matrix.json"
            item = {"name": "control", "binary": "bench_baseline_mutex", "filter": "Pipeline"}
            path.write_text(json.dumps([item, item]))
            with self.assertRaisesRegex(ValueError, "duplicate"):
                diagnose.load_matrix(path)
            item["args"] = ["--benchmark_repetitions=1"]
            path.write_text(json.dumps([item]))
            with self.assertRaisesRegex(ValueError, "recorder controls"):
                diagnose.load_matrix(path)

    def test_matrix_resolves_repository_relative_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "matrix.json"
            path.write_text(json.dumps([{"name": "control", "binary": "build/diagnostic/bench_mutex",
                                         "filter": "Pipeline", "env": {"EXPERIMENT": "fixed"}}]))
            condition = diagnose.load_matrix(path)[0]
            self.assertEqual(condition["binary"], str(recorder.ROOT / "build/diagnostic/bench_mutex"))
            self.assertEqual(condition["prefix"], [])

    def test_first_repetition_marked_as_warmup(self):
        with tempfile.TemporaryDirectory() as directory:
            opts = argparse.Namespace(output=Path(directory), sessions=1, blocks=1, repetitions=3)
            condition = {"name": "control", "binary": "/bin/true", "filter": "Pipeline",
                         "prefix": [], "env": {}, "args": []}
            study = diagnose.DiagnosticStudy(opts, [condition])
            study.raw.mkdir(parents=True)
            sampled = [("BM_Test", repetition, 100.0, 100.0, 1) for repetition in range(3)]
            with patch.object(recorder, "launch", return_value=(b"", [], {"exit_status": 0})), \
                 patch.object(recorder, "samples", return_value=sampled), \
                 patch.object(recorder, "intervals", return_value=[]), \
                 patch.object(recorder, "latency_reports",
                              return_value=[(q, i, {"P50": 7}) for i in range(3) for q in ("queue", "service")]):
                study.one(1, 1, 1, condition, [1, 3], {1, 3}, (0, 0, 0))
            self.assertEqual([row["warmup"] for row in study.rows["samples"]], [1, 0, 0])
            self.assertEqual([(row["quantity"], row["warmup"]) for row in study.rows["latency"]],
                             [("queue", 1), ("service", 1), ("queue", 0), ("service", 0),
                              ("queue", 0), ("service", 0)])

    def test_samples_reject_benchmark_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run.json"
            run = {"name": "BM_Test", "run_name": "BM_Test", "run_type": "iteration", "repetition_index": 0,
                   "real_time": 10.0, "cpu_time": 10.0, "time_unit": "ns", "iterations": 5}
            path.write_text(json.dumps({"benchmarks": [run]}))
            self.assertEqual(recorder.samples(path), [("BM_Test", 0, 10.0, 10.0, 5)])
            failed = {**run, "name": "BM_Failed", "run_name": "BM_Failed", "error_occurred": True,
                      "error_message": "an iteration did not match exactly one order"}
            path.write_text(json.dumps({"benchmarks": [run, failed]}))
            with self.assertRaisesRegex(ValueError, "BM_Failed: an iteration did not match"):
                recorder.samples(path)

    def test_latency_reports_split_by_quantity(self):
        report = ("=== Latency Report: {} (100000 samples, TSC ticks) ===\n"
                  "P50  :    {} ticks\nMax  :   {} ticks\n")
        out = ("--- Pipeline Latency Report (100K orders) ---\n" + report.format("queue", 900000, 990000)
               + report.format("service", 600, 41000) + "  (Trades produced: 7)\n\n"
               + "BM_Pipeline_LatencyReport/iterations:1  1.0 ms\n"
               + report.format("queue", 910000, 995000) + report.format("service", 610, 42000) + "\n")
        self.assertEqual(recorder.latency_reports(out.encode()), [
            ("queue", 0, {"P50": 900000, "Max": 990000}), ("service", 0, {"P50": 600, "Max": 41000}),
            ("queue", 1, {"P50": 910000, "Max": 995000}), ("service", 1, {"P50": 610, "Max": 42000})])
        before = "=== Latency Report (100000 samples) ===\nP50  : 3140000 cycles / 1259 ns\n\n"
        self.assertEqual(recorder.latency_reports(before.encode() * 2),
                         [("end-to-end", 0, {"P50": 3140000}), ("end-to-end", 1, {"P50": 3140000})])

    def test_single_repetition_rejected(self):
        result = subprocess.run([sys.executable, "-B", str(Path(diagnose.__file__)), "--matrix", "unused.json",
                                 "--output", "unused", "--repetitions", "1"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("warm-up", result.stderr)

    def test_cleared_condition_accepts_only_named_unmovable_tasks(self):
        opts = argparse.Namespace(output=Path("/nonexistent"), allow_unmovable=["systemd"])
        study = diagnose.DiagnosticStudy(opts, [])

        def refuse(_baseline, _exclude, failures):
            failures.append((10, 10))
            return (5, 4, 1)

        info = {}
        with patch.object(recorder, "apply_affinity", side_effect=refuse), \
             patch.object(diagnose, "task_name", return_value="systemd"):
            self.assertEqual(study.clear({1, 3}, info), (5, 4, 1))
        self.assertEqual(info["unmovable_tasks"], {"systemd": 1})
        with patch.object(recorder, "apply_affinity", side_effect=refuse), \
             patch.object(diagnose, "task_name", return_value="VBoxHeadless"):
            with self.assertRaisesRegex(RuntimeError, "VBoxHeadless"):
                study.clear({1, 3}, {})

    def test_setup_failure_still_restores_environment_and_affinity(self):
        with tempfile.TemporaryDirectory() as directory:
            opts = argparse.Namespace(output=Path(directory), sessions=1, blocks=1,
                                      repetitions=5, interference="cleared", warmup=0)
            study = diagnose.DiagnosticStudy(opts, [])
            study.raw.mkdir()
            setup = subprocess.CompletedProcess([], 1, "partial setup", "failed")
            restore = subprocess.CompletedProcess([], 0, "restored", "")
            with patch.object(recorder.STATE_FILE.__class__, "exists", return_value=False), \
                 patch.object(diagnose, "privileged", side_effect=[(setup, []), (restore, [])]) as privileged, \
                 patch.object(recorder, "affinity_baseline", return_value={"123": ["1", [0, 1]]}), \
                 patch.object(recorder, "apply_affinity", return_value=(1, 0, 0)) as affinity, \
                 patch.object(recorder, "online_cpus", return_value={0, 1}), \
                 patch.object(diagnose.os, "sched_getaffinity", return_value={0, 1}), \
                 patch.object(diagnose.os, "sched_setaffinity") as restore_self:
                with self.assertRaisesRegex(RuntimeError, "setup failed"):
                    study.session(1)
                self.assertEqual([call.args[0] for call in privileged.call_args_list], ["setup", "restore"])
                self.assertEqual(affinity.call_count, 2)
                restore_self.assert_called_once_with(0, {0, 1})
                self.assertFalse(study.setup_attempted)
                self.assertIsNone(study.baseline)

    def test_signal_during_restore_exits_after_affinity_and_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            opts = argparse.Namespace(output=Path(directory))
            study = diagnose.DiagnosticStudy(opts, [])
            study.raw.mkdir()
            study.baseline = {"123": ["1", [0, 1]]}
            study.original_affinity = {0, 1}
            study.setup_attempted = True
            study.current_session = {"session": 1}
            study.env["sessions"].append(study.current_session)
            restored = subprocess.CompletedProcess([], 0, "restored", "")
            with patch.object(diagnose, "privileged", return_value=(restored, [signal.SIGTERM])), \
                 patch.object(recorder, "apply_affinity", return_value=(1, 0, 0)) as affinity, \
                 patch.object(recorder, "online_cpus", return_value={0, 1}), \
                 patch.object(diagnose.os, "sched_setaffinity") as restore_self:
                with self.assertRaises(SystemExit) as caught:
                    study.leave()
                self.assertEqual(caught.exception.code, 128 + signal.SIGTERM)
                self.assertEqual(affinity.call_count, 2)
                restore_self.assert_called_once_with(0, {0, 1})
                self.assertFalse(study.setup_attempted)
                self.assertIsNone(study.baseline)
            saved = json.loads((study.out / "env.json").read_text())
            self.assertEqual(saved["sessions"][0]["signals_during_restore"], [signal.SIGTERM])
            self.assertEqual(saved["sessions"][0]["affinity_restored"]["not_at_baseline"], 0)

    def test_launch_rejects_nonpositive_sample_interval_before_start(self):
        for interval in (0, -1):
            with self.subTest(interval=interval), \
                 patch.object(recorder.subprocess, "Popen") as popen:
                with self.assertRaisesRegex(ValueError, "sample_interval must be positive"):
                    recorder.launch("dummy", [1], {1}, [], Path("/tmp/unused.json"),
                                    sample_interval=interval)
                popen.assert_not_called()

    def test_launch_past_timeout_is_killed(self):
        process = Mock(pid=12345)
        process.stdout = Mock()
        process.stdout.fileno.return_value = 12
        clock = iter([0, 301 * 10**9])
        with patch.object(recorder, "snapshot", return_value={}), \
             patch.object(recorder.subprocess, "Popen", return_value=process), \
             patch.object(recorder.select, "select", return_value=([], [], [])), \
             patch.object(recorder.time, "monotonic_ns", side_effect=lambda: next(clock)), \
             patch.object(recorder, "kill_launch_session") as kill, \
             patch.object(recorder.os, "set_blocking"), \
             patch.object(recorder.os, "read", side_effect=[b""]):
            with self.assertRaisesRegex(TimeoutError, "after 300 s"):
                recorder.launch("dummy", [1], {1}, [], Path("/tmp/unused.json"), timeout=300)
            kill.assert_called_once_with(12345)
            process.wait.assert_called_once()

    def test_launch_interrupt_kills_group_reaps_and_preserves_output(self):
        process = Mock(pid=12345)
        process.stdout = Mock()
        process.stdout.fileno.return_value = 12
        with patch.object(recorder, "snapshot", return_value={}), \
             patch.object(recorder.subprocess, "Popen", return_value=process) as popen, \
             patch.object(recorder.select, "select", side_effect=KeyboardInterrupt) as poll, \
             patch.object(recorder, "kill_launch_session") as kill, \
             patch.object(recorder.os, "set_blocking"), \
             patch.object(recorder.os, "read", side_effect=[b"partial diagnostic output\n", b""]):
            with self.assertRaises(KeyboardInterrupt) as caught:
                recorder.launch("dummy", [1], {1}, [], Path("/tmp/unused.json"),
                                command_prefix=["setarch", "x86_64", "-R"], env={"TEST": "yes"},
                                sample_interval=0.1)
            poll.assert_called_once_with([12], [], [], 0.1)
            kill.assert_called_once_with(12345)
            process.wait.assert_called_once()
            self.assertEqual(caught.exception.benchmark_output, b"partial diagnostic output\n")
            self.assertTrue(popen.call_args.kwargs["start_new_session"])
            self.assertEqual(popen.call_args.kwargs["env"], {"TEST": "yes"})

    def test_interrupt_stops_child_in_another_process_group(self):
        with tempfile.TemporaryDirectory() as directory:
            wrapper = Path(directory) / "wrapper.py"
            ready = Path(directory) / "ready.json"
            wrapper.write_text(
                "import json, os, subprocess, sys, time\n"
                "from pathlib import Path\n"
                "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], "
                "process_group=0)\n"
                "print('wrapper ready', flush=True)\n"
                "Path(os.environ['DIAGNOSTIC_TEST_READY']).write_text(json.dumps("
                "[os.getpid(), child.pid, os.getsid(child.pid), os.getpgid(child.pid)]))\n"
                "child.wait()\n")
            identities = []

            def interrupt_when_ready(*_args):
                until = time.monotonic() + 3
                while time.monotonic() < until:
                    try:
                        identities[:] = json.loads(ready.read_text())
                        raise KeyboardInterrupt
                    except (FileNotFoundError, json.JSONDecodeError):
                        time.sleep(0.01)
                raise AssertionError("wrapper did not start its child")

            def timeout(_number, _frame):
                raise TimeoutError("launch cancellation blocked")

            previous = signal.signal(signal.SIGALRM, timeout)
            signal.alarm(5)
            try:
                core = min(os.sched_getaffinity(0))
                with patch.object(recorder, "snapshot", return_value={}), \
                     patch.object(recorder.select, "select", side_effect=interrupt_when_ready):
                    with self.assertRaises(KeyboardInterrupt) as caught:
                        recorder.launch(sys.executable, [core], {core}, [], Path(directory) / "unused.json",
                                        command_prefix=[sys.executable, str(wrapper)],
                                        env={**os.environ, "DIAGNOSTIC_TEST_READY": str(ready)})
                parent, child, session, group = identities
                self.assertEqual(session, parent)
                self.assertEqual(group, child)
                self.assertNotEqual(group, parent)
                self.assertIn(b"wrapper ready", caught.exception.benchmark_output)
                # A killed orphan can briefly await init's reap as a zombie;
                # it must have stopped executing before cleanup returns.
                deadline = time.monotonic() + 1
                while time.monotonic() < deadline:
                    try:
                        state = Path(f"/proc/{child}/stat").read_text().rsplit(")", 1)[1].split()[0]
                    except (FileNotFoundError, ProcessLookupError):
                        # Reaped before or while its stat file was read.
                        break
                    if state == "Z":
                        break
                    time.sleep(0.01)
                else:
                    self.fail("child survived launch cancellation")
                with self.assertRaises(ChildProcessError):
                    os.waitpid(parent, os.WNOHANG)
            finally:
                signal.alarm(0)
                signal.signal(signal.SIGALRM, previous)
                # Also clean up the fixture when exercising a broken version.
                for pid in identities[:2]:
                    try:
                        if os.getsid(pid) == identities[0]:
                            os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    unittest.main()

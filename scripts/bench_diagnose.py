#!/usr/bin/env python3
"""Record a rotated matrix of repeat-launch diagnostic conditions.

A block launches every condition once; a session is one setup/restore span.
Each condition is a JSON object with name, binary and filter, and optional
prefix (argv list), env (string overrides) and args (benchmark argv list).
Binary basenames resolve in build/release; other relative paths resolve at
the repository root. Absolute binary paths are also accepted.
Binaries must already be built. No build or global kernel-setting changes are
performed here. See bench_record.py for the shared telemetry definitions.
Command prefixes must keep descendants in the launch session. Creating a new
process group, as GDB does for an inferior, is supported during cancellation.
"""
import argparse
from collections import Counter
import csv
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tarfile
from pathlib import Path

import bench_record as recorder


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_matrix(path):
    matrix = json.loads(path.read_text())
    if not isinstance(matrix, list) or not matrix:
        raise ValueError("matrix must be a nonempty JSON list")
    names = set()
    for condition in matrix:
        if not isinstance(condition, dict):
            raise ValueError("each condition must be an object")
        unknown = set(condition) - {"name", "binary", "filter", "prefix", "env", "args"}
        if unknown:
            raise ValueError(f"unknown condition fields: {sorted(unknown)}")
        name = condition.get("name")
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", name):
            raise ValueError("condition names must contain only letters, numbers, underscores and hyphens")
        if name in names:
            raise ValueError(f"duplicate condition name: {name}")
        names.add(name)
        for field in ("binary", "filter"):
            if not isinstance(condition.get(field), str) or not condition[field] or "\0" in condition[field]:
                raise ValueError(f"{name}: {field} must be a nonempty string")
        binary = Path(condition["binary"])
        if not binary.is_absolute():
            binary = (recorder.BUILD if len(binary.parts) == 1 else recorder.ROOT) / binary
        condition["binary"] = str(binary.resolve())
        for field in ("prefix", "args"):
            values = condition.setdefault(field, [])
            if not isinstance(values, list) or any(not isinstance(v, str) or "\0" in v for v in values):
                raise ValueError(f"{name}: {field} must be a list of strings")
        for arg in condition["args"]:
            if arg.split("=", 1)[0] in {"--benchmark_filter", "--benchmark_repetitions",
                                         "--benchmark_out", "--benchmark_out_format",
                                         "--benchmark_report_aggregates_only", "--benchmark_list_tests"}:
                raise ValueError(f"{name}: recorder controls {arg.split('=', 1)[0]}")
        overrides = condition.setdefault("env", {})
        if not isinstance(overrides, dict) or any(
                not isinstance(k, str) or not k or "=" in k or "\0" in k or
                not isinstance(v, str) or "\0" in v for k, v in overrides.items()):
            raise ValueError(f"{name}: env must map valid environment names to strings")
    return matrix


def task_name(pid):
    try:
        return Path(f"/proc/{pid}/comm").read_text().strip()
    except OSError:
        return "?"


def privileged(action, destination):
    """Finish setup/restore before handling a signal, so they cannot overlap."""
    pending, previous = [], {}
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        previous[sig] = signal.signal(sig, lambda number, _frame: pending.append(number))
    try:
        result = subprocess.run(["sudo", "-n", recorder.INSTALLED, action], cwd=recorder.ROOT,
                                text=True, capture_output=True, start_new_session=True)
        destination.write_text(result.stdout + result.stderr)
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    return result, pending


class DiagnosticStudy:
    def __init__(self, opts, matrix):
        self.opts, self.matrix = opts, matrix
        self.out = opts.output.resolve()
        self.raw = self.out / "raw"
        self.recovery = self.out / "affinity_baseline.json"
        self.rows = {name: [] for name in ("launches", "samples", "intervals", "latency")}
        self.env = {"complete": False, "measurement_mode": True, "sessions": []}
        self.baseline = None
        self.original_affinity = None
        self.setup_attempted = False
        self.current_session = None
        self.owns_output = False

    def prepare(self):
        if recorder.STATE_FILE.exists():
            raise RuntimeError("measurement mode is already active; finish its owning session first")
        if not os.access(recorder.INSTALLED, os.X_OK):
            raise RuntimeError(f"missing installed setup helper: {recorder.INSTALLED}")
        if self.opts.warmup and not os.access(recorder.BUILD / "bench_order_book", os.X_OK):
            raise ValueError("missing bench_order_book warm-up executable; build it before recording")
        for condition in self.matrix:
            binary = Path(condition["binary"])
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise ValueError(f"missing executable {binary}; build it before recording")
            if condition["prefix"] and not shutil.which(condition["prefix"][0]):
                raise ValueError(f"missing prefix command {condition['prefix'][0]}")
        self.out.mkdir(parents=True, exist_ok=False)
        self.owns_output = True
        self.raw.mkdir()
        inputs = {}
        for name in (*recorder.BUILD_INPUTS, "scripts"):
            path = recorder.ROOT / name
            for source in sorted(path.rglob("*")) if path.is_dir() else [path]:
                if source.is_file() and "__pycache__" not in source.parts:
                    inputs[str(source.relative_to(recorder.ROOT))] = sha256(source)
        git = recorder.run("git", "rev-parse", "HEAD")
        if git.returncode:
            raise RuntimeError(git.stderr)
        self.env.update({
            "commit": git.stdout.strip(),
            "git_status": recorder.run("git", "status", "--porcelain").stdout,
            "design": {"sessions": self.opts.sessions, "blocks_per_session": self.opts.blocks,
                       "repetitions": self.opts.repetitions, "interference": self.opts.interference,
                       "allow_unmovable": getattr(self.opts, "allow_unmovable", []),
                       "launch_timeout_s": getattr(self.opts, "launch_timeout", None),
                       "warmup_min_time_s": self.opts.warmup, "sample_interval_s": 0.1,
                       "order": "rotate left by global block index", "matrix": self.matrix},
            "source_sha256": inputs,
            "binaries": {c["binary"]: sha256(Path(c["binary"])) for c in self.matrix},
            "prefix_executable_sha256": {str(Path(shutil.which(c["prefix"][0])).resolve()):
                                          sha256(Path(shutil.which(c["prefix"][0])))
                                          for c in self.matrix if c["prefix"]},
        })
        write_json(self.out / "matrix.json", self.matrix)
        compile_commands = recorder.BUILD / "compile_commands.json"
        if compile_commands.exists():
            shutil.copy2(compile_commands, self.raw / "compile_commands.json")
        recorder.log(f"diagnostic study -> {self.out}")
        self.checkpoint()

    def capture_env(self, name):
        path = self.raw / name
        result = recorder.run("scripts/capture_env.sh", str(path), str(recorder.BUILD))
        if result.returncode:
            (self.raw / f"{name}.error.log").write_text(result.stdout + result.stderr)
            raise RuntimeError(f"environment capture failed: {result.stderr}")
        captured = json.loads(path.read_text())
        captured["kernel_settings"]["randomize_va_space"] = Path(
            "/proc/sys/kernel/randomize_va_space").read_text().strip()
        write_json(path, captured)
        return captured

    def session(self, number):
        if recorder.STATE_FILE.exists():
            raise RuntimeError("measurement mode became active before setup")
        self.original_affinity = set(os.sched_getaffinity(0))
        self.baseline = recorder.affinity_baseline()
        write_json(self.recovery, self.baseline)
        info = {"session": number}
        self.env["sessions"].append(info)
        self.current_session = info
        self.setup_attempted = True
        try:
            result, pending = privileged("setup", self.raw / f"s{number}_setup.txt")
            info["setup_exit_status"] = result.returncode
            if result.returncode:
                raise RuntimeError(f"setup failed: {result.stdout}{result.stderr}")
            if pending:
                raise SystemExit(128 + pending[0])
            listed = recorder.run("scripts/bench_env.sh", "cores", "--list")
            cores = [int(c) for c in listed.stdout.strip().split(",") if c]
            online, measure = recorder.online_cpus(), set(cores)
            if listed.returncode or len(cores) != 2 or not measure <= online or not online - measure:
                raise RuntimeError(f"invalid measurement cores: {listed.stdout}{listed.stderr}")
            os.sched_setaffinity(0, online - measure)
            info.update({"cores": cores, "housekeeping": sorted(online - measure),
                         "before": self.capture_env(f"s{number}_env_before.json")})
            self.checkpoint()
            recorder.log(f"session {number}: cores {cores}, housekeeping {sorted(online - measure)}")
            if self.opts.warmup:
                warmup_path = self.raw / f"s{number}_warmup.json"
                try:
                    output, _, warmup = recorder.launch(
                        "bench_order_book", cores[:1], measure,
                        ["--benchmark_filter=BM_AddOrder_NoMatch", "--benchmark_repetitions=1",
                         f"--benchmark_min_time={self.opts.warmup}s"], warmup_path, sample_interval=0.1)
                except BaseException as exc:
                    warmup_path.with_suffix(".log").write_bytes(getattr(exc, "benchmark_output", b""))
                    raise
                warmup_path.with_suffix(".log").write_bytes(output)
                info["warmup"] = warmup
                if warmup["exit_status"]:
                    raise RuntimeError(f"session {number} warm-up failed")
            for block in range(1, self.opts.blocks + 1):
                offset = ((number - 1) * self.opts.blocks + block - 1) % len(self.matrix)
                order = self.matrix[offset:] + self.matrix[:offset]
                for position, condition in enumerate(order, 1):
                    moved = self.clear(measure, info) if self.opts.interference == "cleared" else (0, 0, 0)
                    self.one(number, block, position, condition, cores, measure, moved)
                recorder.log(f"session {number} block {block}/{self.opts.blocks} complete")
        finally:
            try:
                if "before" in info:
                    info["after"] = self.capture_env(f"s{number}_env_after.json")
            finally:
                self.leave()

    def clear(self, measure, info):
        """Restrict this user's tasks to the housekeeping cores. Only tasks of
        processes named with --allow-unmovable may refuse; they are recorded."""
        refused = []
        moved = recorder.apply_affinity(self.baseline, measure, refused)
        names = Counter(task_name(pid) for pid, _ in refused)
        unexpected = sorted(set(names) - set(getattr(self.opts, "allow_unmovable", [])))
        if unexpected:
            raise RuntimeError(f"could not clear {moved[2]} user tasks from measurement cores: "
                               + ", ".join(unexpected))
        recorded = info.setdefault("unmovable_tasks", {})
        for name, count in names.items():
            recorded[name] = max(recorded.get(name, 0), count)
        return moved

    def one(self, session, block, position, condition, cores, measure, moved):
        # The absolute output argument has constant length throughout a study.
        # Changing argv lengths can change stack addresses even with ASLR off.
        stem = (f"s{session:0{len(str(self.opts.sessions))}d}_"
                f"b{block:0{len(str(self.opts.blocks))}d}_"
                f"c{self.matrix.index(condition):0{len(str(len(self.matrix)))}d}")
        json_path = self.raw / f"{stem}.json"
        args = [f"--benchmark_filter={condition['filter']}",
                f"--benchmark_repetitions={self.opts.repetitions}", *condition["args"]]
        command = ["taskset", "-c", ",".join(map(str, sorted(cores))), *condition["prefix"],
                   condition["binary"], "--benchmark_out_format=json", f"--benchmark_out={json_path}", *args]
        environment = {**os.environ, **condition["env"]}
        environment_bytes = b"\0".join(os.fsencode(f"{key}={value}") for key, value in environment.items()) + b"\0"
        metadata = {"condition": condition["name"], "command": command, "env_overrides": condition["env"],
                    "environment": "inherits recorder environment, then applies env_overrides",
                    "environment_entries": len(environment), "environment_bytes": len(environment_bytes),
                    "environment_sha256": hashlib.sha256(environment_bytes).hexdigest(),
                    "complete": False}
        meta_path = self.raw / f"{stem}.command.json"
        write_json(meta_path, metadata)
        key = {"session": session, "block": block, "condition": condition["name"],
               "binary": Path(condition["binary"]).name}
        try:
            output, marks, record = recorder.launch(
                condition["binary"], cores, measure, args, json_path,
                command_prefix=condition["prefix"], env=environment, sample_interval=0.1,
                timeout=getattr(self.opts, "launch_timeout", None))
        except BaseException as exc:
            (self.raw / f"{stem}.log").write_bytes(getattr(exc, "benchmark_output", b""))
            metadata["error"] = f"{type(exc).__name__}: {exc}"
            write_json(meta_path, metadata)
            raise
        (self.raw / f"{stem}.log").write_bytes(output)
        metadata.update({"complete": True, "telemetry": record})
        write_json(meta_path, metadata)
        self.rows["launches"].append({**key, "position": position, "cores": " ".join(map(str, cores)),
                                      "user_tasks_seen": moved[0], "user_tasks_moved": moved[1],
                                      "user_tasks_unmovable": moved[2], **record})
        self.checkpoint()
        if record["exit_status"]:
            raise RuntimeError(f"{stem} exited {record['exit_status']}; see its raw log")
        sampled = recorder.samples(json_path)
        counts = {}
        for name, repetition, real, cpu, iterations in sampled:
            counts.setdefault(name, set()).add(repetition)
            self.rows["samples"].append({**key, "benchmark": name, "repetition": repetition,
                                         "real_time_ns": repr(real), "cpu_time_ns": repr(cpu),
                                         "iterations": iterations})
        if not sampled or any(reps != set(range(self.opts.repetitions)) for reps in counts.values()):
            raise RuntimeError(f"{stem} did not produce the requested repetitions")
        for name, wall, cpu, waited in recorder.intervals(output, marks):
            self.rows["intervals"].append({**key, "benchmark": name, "wall_s": round(wall, 4),
                                           "cpu_s": round(cpu, 4),
                                           "cpu_per_wall": round(cpu / wall, 4) if wall else "",
                                           "waited_ms": round(waited, 3)})
        for index, report in enumerate(recorder.latency_reports(output)):
            for percentile, ticks in report.items():
                self.rows["latency"].append({**key, "report": index, "percentile": percentile, "tsc_ticks": ticks})
        self.checkpoint()

    def leave(self):
        errors, pending = [], []
        info = self.current_session
        if self.setup_attempted:
            try:
                result, pending = privileged("restore", self.raw / f"s{info['session']}_restore.txt")
                info["restore_exit_status"] = result.returncode
                info["signals_during_restore"] = pending
                if result.returncode:
                    errors.append(f"restore failed: {result.stdout}{result.stderr}")
                else:
                    self.setup_attempted = False
            except BaseException as exc:
                errors.append(f"restore failed: {exc}")
        if self.baseline is not None:
            try:
                recorder.apply_affinity(self.baseline)
                seen, wrong, failed = recorder.apply_affinity(self.baseline)
                os.sched_setaffinity(0, self.original_affinity & recorder.online_cpus())
                info["affinity_restored"] = {"tasks": seen, "not_at_baseline": wrong, "not_changeable": failed}
                if wrong or failed:
                    errors.append(f"affinity restoration left {wrong} incorrect and {failed} unchangeable tasks")
                if not errors:
                    self.recovery.unlink(missing_ok=True)
                    self.baseline = None
            except BaseException as exc:
                errors.append(f"affinity restoration failed: {exc}")
        if errors:
            info["cleanup_errors"] = errors
        if pending:
            self.checkpoint()
            raise SystemExit(128 + pending[0])
        if errors:
            raise RuntimeError("; ".join(errors))

    def checkpoint(self):
        for name, rows in self.rows.items():
            if rows:
                with (self.out / f"{name}.csv").open("w", newline="") as handle:
                    writer = csv.DictWriter(handle, fieldnames=list(rows[0]), lineterminator="\n")
                    writer.writeheader()
                    writer.writerows(rows)
        write_json(self.out / "env.json", self.env)

    def archive(self):
        self.checkpoint()
        # Keep raw/ as a recovery path if compression itself is interrupted.
        temporary = self.out / "raw.tar.gz.tmp"
        with tarfile.open(temporary, "w:gz") as archive:
            archive.add(self.raw, arcname="raw")
        temporary.replace(self.out / "raw.tar.gz")
        shutil.rmtree(self.raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sessions", type=int, default=2)
    parser.add_argument("--blocks", type=int, default=8)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=3, help="minimum seconds of discarded single-core warm-up")
    parser.add_argument("--interference", choices=("cleared", "shared"), default="cleared")
    parser.add_argument("--launch-timeout", type=float, default=300, metavar="SECONDS",
                        help="kill a launch still running after this long and stop the study "
                             "(default 300); the mutex baseline can hang on a lost wake-up")
    parser.add_argument("--allow-unmovable", action="append", default=[], metavar="NAME",
                        help="process name (as in /proc/PID/comm) whose tasks may refuse the cleared "
                             "affinity, such as a service manager holding a capability; recorded per "
                             "session, and any other refusal stops the study")
    opts = parser.parse_args()
    if min(opts.sessions, opts.blocks, opts.repetitions) < 1:
        parser.error("sessions, blocks and repetitions must all be positive")
    if opts.warmup < 0:
        parser.error("warmup must be nonnegative")
    if not opts.launch_timeout > 0:
        parser.error("launch timeout must be positive")
    try:
        matrix = load_matrix(opts.matrix)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))

    def stop(number, _frame):
        raise SystemExit(128 + number)

    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)
    study = DiagnosticStudy(opts, matrix)
    try:
        study.prepare()
        for number in range(1, opts.sessions + 1):
            study.session(number)
        study.env["complete"] = True
    except BaseException as exc:
        study.env["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        if study.owns_output and study.raw.exists():
            study.archive()
    recorder.log(f"recorded {len(study.rows['launches'])} launches -> {study.out}")


if __name__ == "__main__":
    main()

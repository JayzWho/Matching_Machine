#!/usr/bin/env python3
"""Hold measurement mode for one command that the user runs as root.

    python3 scripts/diagnostics/root_session.py enter DIRECTORY
    (the user runs the printed command as root)
    python3 scripts/diagnostics/root_session.py leave DIRECTORY

enter records this user's task affinities, runs `mm-bench-env setup` through
its password-free rule, moves this user's tasks off the two measurement cores
as the `cleared` condition of bench_diagnose.py does, captures the
environment, and prints the command for the root run of the queue-mechanism
program. leave restores the affinities, runs `mm-bench-env restore` and
captures the environment again. Nothing else runs with privilege.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "record"))
import bench_record as recorder  # noqa: E402

BINARY = recorder.ROOT / "build/diagnostics/mechanism/bench_queue_mechanism"
FILTER = "^BM_(S1|OneWay|LineRoundTrip|TimesFar)/"


def privileged(action, directory):
    result = subprocess.run(["sudo", "-n", recorder.INSTALLED, action], cwd=recorder.ROOT,
                            text=True, capture_output=True)
    (directory / f"{action}.txt").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(f"{action} failed: {result.stdout}{result.stderr}")


def capture(directory, name):
    path = directory / name
    result = recorder.run("scripts/env/capture_env.sh", str(path), str(recorder.BUILD))
    if result.returncode:
        raise SystemExit(f"environment capture failed: {result.stderr}")


def enter(directory):
    if recorder.STATE_FILE.exists():
        raise SystemExit("measurement mode is already active")
    directory.mkdir(parents=True, exist_ok=False)
    baseline = recorder.affinity_baseline()
    (directory / "affinity_baseline.json").write_text(json.dumps(baseline) + "\n")
    privileged("setup", directory)
    listed = recorder.run("scripts/env/bench_env.sh", "cores", "--list")
    cores = [int(c) for c in listed.stdout.strip().split(",") if c]
    if listed.returncode or len(cores) != 2:
        raise SystemExit(f"invalid measurement cores: {listed.stdout}{listed.stderr}")
    refused = []
    seen, moved, failed = recorder.apply_affinity(baseline, set(cores), refused)
    names = sorted({Path(f"/proc/{pid}/comm").read_text().strip() for pid, _ in refused
                    if Path(f"/proc/{pid}/comm").exists()})
    capture(directory, "env_before.json")
    command = ["sudo", "taskset", "-c", ",".join(map(str, sorted(cores))), str(BINARY),
               f"--benchmark_filter={FILTER}", "--benchmark_repetitions=3",
               "--benchmark_enable_random_interleaving=true", "--benchmark_out_format=json",
               f"--benchmark_out={directory.resolve() / 'root.json'}"]
    text = shlex.join(command) + f" > {shlex.quote(str(directory.resolve() / 'root.log'))} 2>&1"
    (directory / "command.json").write_text(json.dumps(
        {"cores": cores, "tasks_seen": seen, "tasks_moved": moved, "tasks_refused": failed,
         "refusing": names, "command": text, "binary_sha256": hashlib.sha256(BINARY.read_bytes()).hexdigest(),
         "git_commit": recorder.run("git", "rev-parse", "HEAD").stdout.strip()}, indent=2) + "\n")
    print(f"cores {cores}; {moved} of {seen} tasks moved; refused by {names or 'none'}")
    print(text)


def leave(directory):
    baseline = json.loads((directory / "affinity_baseline.json").read_text())
    recorder.apply_affinity(baseline)
    seen, wrong, failed = recorder.apply_affinity(baseline)
    privileged("restore", directory)
    capture(directory, "env_after.json")
    print(f"affinities restored: {seen} tasks, {wrong} not at baseline, {failed} unchangeable")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("enter", "leave"))
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    (enter if args.action == "enter" else leave)(args.directory)


if __name__ == "__main__":
    main()

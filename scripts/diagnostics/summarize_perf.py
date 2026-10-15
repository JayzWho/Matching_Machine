#!/usr/bin/env python3
"""Write whole-launch perf counters and condition medians for a completed study."""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import re
from statistics import median

import summarize

EVENTS = ("cycles", "instructions", "branches", "branch-misses", "cache-references",
          "cache-misses", "context-switches", "cpu-migrations", "page-faults",
          "mem_load_l3_hit_retired.xsnp_fwd", "l2_rqsts.all_rfo")


def report(directory, output):
    summarize.summarize(directory)  # Refuse incomplete or malformed evidence.
    launches = summarize.read_csv(directory / "launches.csv")
    orders = defaultdict(int)
    for row in summarize.read_csv(directory / "samples.csv"):
        match = re.fullmatch(r"BM_MutexPipeline_Throughput/(10000|50000|100000)/iterations:3",
                             row["benchmark"])
        if not match:
            raise ValueError(f"unexpected workload: {row['benchmark']}")
        orders[summarize.launch_key(row)] += int(match[1]) * int(row["iterations"])
    files, rows = summarize.raw_files(directory), []
    for name, data in sorted(files.items()):
        identity = re.fullmatch(r"s(\d+)_b(\d+)_c\d+\.command\.json", name)
        if not identity:
            continue
        metadata = json.loads(data)
        if not metadata.get("complete"):
            raise ValueError(f"incomplete command: {name}")
        session, block = map(int, identity.groups())
        matching = [row for row in launches if summarize.launch_key(row)[:3] ==
                    (session, block, metadata["condition"])]
        if len(matching) != 1:
            raise ValueError(f"command does not identify exactly one launch: {name}")
        launch = matching[0]
        key = summarize.launch_key(launch)
        if orders[key] != 2_400_000:
            raise ValueError(f"expected 2,400,000 workload orders in {name}, found {orders[key]}")
        row = {"session": session, "block": block, "condition": metadata["condition"],
               "binary": launch["binary"], "workload_orders": orders[key],
               **{field: launch[field] for field in ("mhz_samples", "mhz_min", "mhz_max")}}
        log = files[name.removesuffix(".command.json") + ".log"].decode(errors="replace")
        for fields in csv.reader(log.splitlines()):
            if len(fields) < 5 or fields[2] not in EVENTS:
                continue
            event = fields[2]
            if event in row:
                raise ValueError(f"duplicate perf event {event} in {name}")
            row[event] = int(fields[0])  # Unsupported/not-counted events must fail.
            row[event + "_runtime_ns"] = int(fields[3])
            row[event + "_running_pct"] = float(fields[4])
            if not 0 < row[event + "_running_pct"] <= 100:
                raise ValueError(f"invalid running percentage in {name}: {fields}")
        if any(event not in row for event in EVENTS):
            raise ValueError(f"missing perf events in {name}")
        rows.append(row)
    identities = [(row["session"], row["block"], row["condition"]) for row in rows]
    if len(rows) != len(launches) or len(set(identities)) != len(rows):
        raise ValueError("perf logs do not cover every launch exactly once")
    output.mkdir(parents=True, exist_ok=True)
    with (output / "perf_counters.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0], lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    conditions = sorted({row["condition"] for row in rows})
    running = [row[event + "_running_pct"] for row in rows for event in EVENTS]
    lines = ["# Whole-launch hardware and software counters", "",
             "Counters cover each complete profiled process, including startup, allocation, "
             "thread creation and all three throughput sizes. They are not isolated 100,000-order counters.", "",
             "Each launch processes 5 repetitions × 3 iterations × (10,000 + 50,000 + 100,000) "
             "= 2,400,000 workload orders. The per-order column divides whole-launch counts by "
             "that denominator; it retains process overhead. `cycles` are hardware core cycles.", "",
             f"Perf reports running percentages from {min(running):.2f}% to {max(running):.2f}% "
             f"across {len(rows)} launches and {len(EVENTS)} events.", ""]
    header = ["Event"]
    for condition in conditions:
        count = sum(row["condition"] == condition for row in rows)
        header += [f"{condition}: launch median (n={count})", f"{condition}: median per order"]
    lines += ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    for event in EVENTS:
        cells = [f"`{event}`"]
        for condition in conditions:
            value = median(row[event] for row in rows if row["condition"] == condition)
            cells += [f"{value:,.1f}", f"{value / 2_400_000:.6g}"]
        lines.append("| " + " | ".join(cells) + " |")
    lines += ["", "## Measurement telemetry", "",
              "Frequency samples are scheduler-derived readings and may include a core becoming idle. "
              "Their minimum does not independently establish the frequency while both workers are busy.", "",
              "Derived process CPU and foreign CPU telemetry is omitted: perf starts the benchmark as "
              "a child, and the recorder's process-clock and wait4 accounting has not been validated "
              "for that wrapper. Frequency, interrupt and throttle snapshots remain applicable.", "",
              "| Condition | Launches with frequency samples | Samples per launch | MHz min–max | "
              "Throttle events | Device IRQs | Suspended seconds |",
              "|---|---:|---:|---:|---:|---:|---:|"]
    for condition in conditions:
        mine = [row for row in launches if row["condition"] == condition]
        counts = [int(row["mhz_samples"]) for row in mine]
        mhz = [float(row[field]) for row in mine for field in ("mhz_min", "mhz_max") if row[field]]
        throttle = sum(int(row["throttle_core"]) + int(row["throttle_package"]) for row in mine)
        lines.append(f"| {condition} | {sum(n > 0 for n in counts)}/{len(mine)} | {min(counts)}–{max(counts)} "
                     f"| {min(mhz):.3f}–{max(mhz):.3f} | {throttle} "
                     f"| {sum(int(row['device_irqs']) for row in mine)} "
                     f"| {sum(float(row['suspended_s']) for row in mine):.3f} |")
    result = "\n".join(lines) + "\n"
    (output / "PERF.md").write_text(result)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("study", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--stdout", action="store_true", help="also print the Markdown report")
    args = parser.parse_args()
    result = report(args.study, args.output or args.study / "artifacts")
    if args.stdout:
        print(result, end="")

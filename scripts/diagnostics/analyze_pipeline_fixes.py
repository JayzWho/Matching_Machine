#!/usr/bin/env python3
"""Tables of the validation recording of the pipeline fixes.

Usage: scripts/diagnostics/analyze_pipeline_fixes.py STUDY_DIR > tables.md

Conditions: spsc-before, spsc-pinned, spsc-pinned-pool, mutex-before and
mutex-pinned (see build_pipeline_variants.sh). A launch is represented by the
median of its counted repetitions (all but the warm-up). Differences between
conditions are differences of the medians of their launch medians; p is the
share of 20,000 shufflings of the launch medians between the two conditions
that give at least that difference (fixed seed).
"""
import csv
import json
import random
import statistics as st
import sys
from collections import defaultdict
from pathlib import Path

TAIL = 4_000_000          # TSC ticks; the variance-sources study's mark of a tail event
FAMILIES = (("BM_Pipeline_Throughput", ("spsc-before", "spsc-pinned", "spsc-pinned-pool")),
            ("BM_MutexPipeline_Throughput", ("mutex-before", "mutex-pinned")))
SEED, SHUFFLES = 20261027, 20000


def read(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def base(name):
    return name.removesuffix("/real_time")


def cv(values):
    return st.stdev(values) / st.mean(values) * 100 if len(values) > 1 else float("nan")


def table(header, rows):
    return ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)] + \
           ["| " + " | ".join(map(str, row)) + " |" for row in rows] + [""]


def shuffle_p(a, b, rng):
    observed = abs(st.median(a) - st.median(b))
    pooled, hits = a + b, 0
    for _ in range(SHUFFLES):
        rng.shuffle(pooled)
        hits += abs(st.median(pooled[:len(a)]) - st.median(pooled[len(a):])) >= observed
    return (hits + 1) / (SHUFFLES + 1)


def main():
    study = Path(sys.argv[1])
    env = json.loads((study / "env.json").read_text())
    reps = defaultdict(lambda: defaultdict(dict))   # (condition, benchmark) -> launch -> repetition -> ns
    for row in read(study / "samples.csv"):
        launch = (int(row["session"]), int(row["block"]))
        reps[(row["condition"], base(row["benchmark"]))][launch][int(row["repetition"])] = float(row["real_time_ns"])
    warmup = env["design"]["warmup_repetitions"]
    out = [f"Study `{study.name}`: {env['design']['sessions']} sessions × {env['design']['blocks_per_session']} "
           f"blocks, {env['design']['repetitions']} repetitions per launch, the first {warmup} a warm-up.", ""]

    # ── throughput ──
    medians, rows = {}, []
    for (condition, name), launches in sorted(reps.items(), key=lambda item: (item[0][1], item[0][0])):
        if "Throughput" not in name:
            continue
        counted = [[v for r, v in sorted(by_rep.items()) if r >= warmup] for by_rep in launches.values()]
        meds = [st.median(c) / 1e6 for c in counted]
        medians[(condition, name)] = meds
        rows.append([f"`{name}`", condition, len(meds), f"{st.median(meds):.3f}", f"{min(meds):.3f}",
                     f"{max(meds):.3f}", f"{cv(meds):.1f}%", f"{st.median(cv(c) for c in counted):.1f}%"])
    out += ["## Throughput, ms per run of the pipeline", ""]
    out += table(["Benchmark", "Condition", "Launches", "Median", "Lowest launch", "Highest launch",
                  "CV of launch medians", "CV inside a launch, median"], rows)

    rng = random.Random(SEED)
    names = sorted({name for _, name in medians}, key=lambda n: (n.split("/")[0], int(n.split("/")[1])))
    out += ["## Change of the median between conditions", "",
            "Each cell is the column's median relative to the row's: negative means the column is faster. "
            "p from shuffling the launch medians of the two conditions.", ""]
    for family, conditions in FAMILIES:
        for name in (n for n in names if n.startswith(family + "/")):
            rows = []
            for first in conditions:
                cells = []
                for second in conditions:
                    a, b = medians[(first, name)], medians[(second, name)]
                    cells.append("—" if first == second else
                                 f"{(st.median(b) / st.median(a) - 1) * 100:+.1f}% (p {shuffle_p(a, b, rng):.3f})")
                rows.append([first, *cells])
            out += [f"`{name}`", ""] + table(["", *conditions], rows)

    # ── warm-up ──
    rows = []
    for (condition, name), launches in sorted(reps.items(), key=lambda item: (item[0][1], item[0][0])):
        if "Throughput" not in name:
            continue
        ratios = [by_rep[0] / st.median(v for r, v in by_rep.items() if r >= warmup) for by_rep in launches.values()]
        slowest = sum(by_rep[0] == max(by_rep.values()) for by_rep in launches.values())
        with_first = st.median(cv(list(by_rep.values())) for by_rep in launches.values())
        without = st.median(cv([v for r, v in by_rep.items() if r >= warmup]) for by_rep in launches.values())
        rows.append([f"`{name}`", condition, f"{st.median(ratios):.2f}×", f"{min(ratios):.2f}× to {max(ratios):.2f}×",
                     f"{slowest} of {len(ratios)}", f"{with_first:.1f}%", f"{without:.1f}%"])
    out += ["## First repetition", "",
            "The first repetition relative to the median of the others in the same launch.", ""]
    out += table(["Benchmark", "Condition", "Median ratio", "Range", "Slowest repetition of its launch",
                  "CV inside a launch, all", "without the first"], rows)

    # ── latency ──
    reports = defaultdict(lambda: defaultdict(dict))  # condition -> (launch, report) -> percentile -> ticks
    flags = {}
    for row in read(study / "latency.csv"):
        key = ((int(row["session"]), int(row["block"])), int(row["report"]))
        reports[row["condition"]][key][row["percentile"]] = int(row["tsc_ticks"])
        flags[(row["condition"], key)] = int(row["warmup"])
    rows = []
    for condition in sorted(reports):
        counted = [r for k, r in reports[condition].items() if not flags[(condition, k)]]
        first = [r for k, r in reports[condition].items() if flags[(condition, k)]]
        rows.append([condition, len(counted),
                     f"{st.median(r['P50'] for r in counted):,.0f}", f"{st.median(r['P99'] for r in counted):,.0f}",
                     f"{st.median(r['Max'] for r in counted):,.0f}", f"{max(r['Max'] for r in counted):,.0f}",
                     sum(r["Max"] > TAIL for r in counted), f"{sum(r['Max'] > TAIL for r in first)} of {len(first)}"])
    out += ["## Latency reports at 100,000 orders, TSC ticks", "",
            f"A tail event is a report whose maximum exceeds {TAIL:,} ticks.", ""]
    out += table(["Condition", "Reports counted", "P50, median", "P99, median", "Max, median", "Max, highest",
                  "Tail events", "Tail events in warm-up reports"], rows)

    # ── calibration and telemetry ──
    rows = []
    for session in env["sessions"]:
        for when in ("before", "after"):
            c = session.get(when, {}).get("tsc", {}).get("calibration") or {}
            rows.append([session["session"], when, c.get("ghz"), c.get("ghz_min"), c.get("ghz_max")])
    out += ["## TSC calibration against CLOCK_MONOTONIC, GHz", ""]
    out += table(["Session", "When", "Median", "Lowest interval", "Highest interval"], rows)
    print("\n".join(out))


if __name__ == "__main__":
    main()

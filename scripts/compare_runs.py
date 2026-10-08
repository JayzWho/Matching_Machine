#!/usr/bin/env python3
"""Compare benchmark runs recorded by run_anchor.sh.

Usage:
    scripts/compare_runs.py RUN_DIR RUN_DIR [RUN_DIR ...]  > comparison.md

Each RUN_DIR is a directory written by scripts/run_anchor.sh. Runs are labelled
A, B, C, ... in the order given. Output is Markdown on stdout, computed only
from the raw files in those directories (Google Benchmark JSON, logs,
env_before.json, RESULT.md), so the document can be regenerated and checked.

What it reports, and why:
  - benchmarks grouped into families that share a workload, because families
    reproduce very differently and an average over all of them hides that;
  - per family, the median of every benchmark in every run;
  - per family, a square matrix with one row and one column per run. A cell
    off the diagonal is the change of the row's run relative to the column's
    run, so every pair of runs is compared and no run is singled out as the
    reference. A cell on the diagonal is the coefficient of variation (CV)
    inside that run: how tightly its own repetitions agree. Reading along a
    row puts the two side by side, which shows whether the spread inside a run
    predicts the distance to another run;
  - the pipeline latency percentiles, taken from the logs, in the same form.
"""
import json
import os
import re
import statistics as st
import sys

SUITES = ["bench_order_book", "bench_spsc_ring_buffer", "bench_matching_engine", "bench_baseline_mutex"]
NS_PER = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}
LATENCY_LOGS = (("SPSC pipeline", "bench_matching_engine.log"), ("Mutex pipeline", "bench_baseline_mutex.log"))

# (title, threads in the timed region, predicate on (suite, benchmark name)).
# Google Benchmark reports threads=1 for all of them (it does not see threads
# the benchmark creates), so membership goes by suite and name.
FAMILIES = [
    ("Order book and matching", 1,
     lambda s, n: s == "bench_order_book" or n == "BM_SingleThread_Baseline"),
    ("Ring buffer push and pop", 1,
     lambda s, n: n.startswith("BM_SPSC_SingleThread_")),
    ("SPSC ring buffer, producer to consumer", 2,
     lambda s, n: n.startswith(("BM_SPSC_ProducerConsumer", "BM_SPSC_Order_ProducerConsumer"))),
    ("Mutex queue, producer to consumer", 2,
     lambda s, n: n.startswith("BM_Mutex_ProducerConsumer")),
    ("SPSC pipeline", 2,
     lambda s, n: n.startswith("BM_Pipeline_Throughput")),
    ("Mutex pipeline", 2,
     lambda s, n: n.startswith("BM_MutexPipeline_Throughput")),
]
# Their timed region is the sorting and printing of the latency report; the
# pipeline itself runs with the timer paused. Their output is the report.
NOT_TIMED = "_LatencyReport"


def load_run(path):
    run = {"dir": os.path.basename(os.path.normpath(path)), "median": {}, "cv": {}}
    for suite in SUITES:
        with open(os.path.join(path, suite + ".json")) as f:
            for b in json.load(f)["benchmarks"]:
                if b["run_type"] != "aggregate":
                    continue
                key = (suite, b["run_name"])
                if b["aggregate_name"] == "median":
                    run["median"][key] = b["real_time"] * NS_PER[b["time_unit"]]
                elif b["aggregate_name"] == "cv":
                    run["cv"][key] = b["real_time"] * 100.0
    with open(os.path.join(path, "env_before.json")) as f:
        env = json.load(f)
    run["kernel"] = env.get("os", {}).get("kernel", "?")
    run["commit"] = env.get("git", {}).get("short", "?")
    run["cores"] = env.get("measurement", {}).get("cores", "?")
    run["when"] = env.get("captured_at", "?")[:16].replace("T", " ")
    with open(os.path.join(path, "RESULT.md")) as f:
        result = f.read()
    m = re.search(r"\*\*Verdict: (\w+)\*\*", result)
    run["verdict"] = m.group(1) if m else "?"
    m = re.search(r"throttle delta: (.+)", result)
    run["throttle"] = m.group(1).strip() if m else "?"
    run["latency"] = {}
    for name, log in LATENCY_LOGS:
        with open(os.path.join(path, log)) as f:
            text = f.read()
        reports = re.findall(r"=== Latency Report \(\d+ samples\) ===\n(.*?)(?:\n\n|\Z)", text, re.S)
        for body in reports:
            for k, v in re.findall(r"^(P50|P99)\s*:\s*(\d+) cycles", body, re.M):
                run["latency"].setdefault((name, k), []).append(int(v))
    return run


def pct_change(new, old):
    return (new / old - 1.0) * 100.0


def fmt_pct(value):
    """Signed percentage; a change that rounds to zero is shown without a sign."""
    return "0.0%" if abs(value) < 0.05 else f"{value:+.1f}%"


def fmt_range(values):
    lo, hi = fmt_pct(min(values)), fmt_pct(max(values))
    return lo if lo == hi else f"{lo} to {hi}"


def fmt_sig(value):
    """Four significant figures, with thousands separators."""
    if value >= 1000:
        return f"{value:,.0f}"
    digits = 3 - len(str(int(value))) + 1
    return f"{value:.{max(digits, 0)}f}"


def time_unit(values_ns):
    low = min(values_ns)
    if low < 1e4:
        return "ns", 1.0
    if low < 1e7:
        return "us", 1e3
    return "ms", 1e6


def value_table(labels, rows):
    """rows: (name, unit, [value per run], largest CV)."""
    out = ["| Benchmark | Unit | " + " | ".join(labels) + " | Spread | Largest within-run CV |",
           "|---|---|" + "---:|" * (len(labels) + 2)]
    for name, unit, values, cv in rows:
        spread = (max(values) / min(values) - 1.0) * 100.0
        out.append(f"| `{name}` | {unit} | " + " | ".join(fmt_sig(v) for v in values) + f" | {spread:.1f}% | {cv:.1f}% |")
    return out + [""]


def matrix(labels, medians, cvs):
    """medians, cvs: one list per benchmark, each with one value per run."""
    out = ["| | " + " | ".join(f"vs {lab}" for lab in labels) + " |", "|---|" + "---:|" * len(labels)]
    for i, row in enumerate(labels):
        cells = []
        for j in range(len(labels)):
            if i == j:
                cells.append(f"*CV {max(cv[i] for cv in cvs):.1f}%*")
            else:
                cells.append(fmt_range([pct_change(m[i], m[j]) for m in medians]))
        out.append(f"| **{row}** | " + " | ".join(cells) + " |")
    return out + [""]


def main(paths):
    runs = [load_run(p) for p in paths]
    labels = [chr(ord("A") + i) for i in range(len(runs))]
    out = []

    out += ["## Runs", "", "| Run | Directory | Recorded | Commit | Kernel | Cores | Verdict | Throttle delta |",
            "|---|---|---|---|---|---|---|---|"]
    for lab, r in zip(labels, runs):
        out.append(f"| {lab} | `{r['dir']}` | {r['when']} | `{r['commit']}` | {r['kernel']} | {r['cores']} | {r['verdict']} | {r['throttle']} |")
    out.append("")

    keys = [k for k in runs[0]["median"] if all(k in r["median"] for r in runs) and NOT_TIMED not in k[1]]
    families = []
    for title, threads, member in FAMILIES:
        group = [k for k in keys if member(*k)]
        if group:
            families.append((f"{title} ({'one thread' if threads == 1 else 'two threads'}, {len(group)} benchmarks)", group))
    unassigned = [k[1] for k in keys if not any(k in g for _, g in families)]
    if unassigned:
        sys.exit("compare_runs.py: benchmarks without a family: " + ", ".join(unassigned))

    values, matrices = [], []
    for title, group in families:
        rows, medians, cvs = [], [], []
        for key in group:
            med = [r["median"][key] for r in runs]
            cv = [r["cv"].get(key, float("nan")) for r in runs]
            unit, div = time_unit(med)
            rows.append((key[1], unit, [m / div for m in med], max(cv)))
            medians.append(med)
            cvs.append(cv)
        values += [f"### {title}", ""] + value_table(labels, rows)
        matrices += [f"### {title}", ""] + matrix(labels, medians, cvs)

    for name, _ in LATENCY_LOGS:
        rows, medians, cvs = [], [], []
        for pct in ("P50", "P99"):
            samples = [r["latency"][(name, pct)] for r in runs]
            med = [st.median(s) for s in samples]
            cv = [st.stdev(s) / st.mean(s) * 100.0 for s in samples]
            rows.append((f"{name} {pct}", "TSC ticks", med, max(cv)))
            medians.append(med)
            cvs.append(cv)
        title = f"{name} latency at 100,000 orders (P50 and P99)"
        values += [f"### {title}", ""] + value_table(labels, rows)
        matrices += [f"### {title}", ""] + matrix(labels, medians, cvs)

    out += ["## Measurements", ""] + values
    out += ["## Differences between runs", ""] + matrices

    def med_of(run, suite, name):
        return run["median"].get((suite, name))
    out += ["## Pipeline throughput at 100,000 orders (from real time)", "",
            "| Run | SPSC pipeline | Mutex pipeline | SPSC / mutex |", "|---|---:|---:|---:|"]
    for lab, r in zip(labels, runs):
        a = med_of(r, "bench_matching_engine", "BM_Pipeline_Throughput/100000/iterations:3")
        b = med_of(r, "bench_baseline_mutex", "BM_MutexPipeline_Throughput/100000/iterations:3")
        if a and b:
            out.append(f"| {lab} | {100000 / (a / 1e9) / 1e6:.2f} M orders/s | {100000 / (b / 1e9) / 1e6:.2f} M orders/s | {b / a:.2f}× |")
    out.append("")
    print("\n".join(out))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        sys.exit(1)
    main(sys.argv[1:])

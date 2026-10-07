#!/usr/bin/env python3
"""Compare benchmark runs recorded by run_anchor.sh.

Usage:
    scripts/compare_runs.py RUN_DIR RUN_DIR [RUN_DIR ...]  > comparison.md

Each RUN_DIR is a directory written by scripts/run_anchor.sh. The first one is
the reference. Output is Markdown on stdout, computed only from the raw files
in those directories (Google Benchmark JSON, logs, env_before.json, RESULT.md),
so the document can be regenerated and checked.

What it reports, and why:
  - per benchmark, the median of each run and its change against the reference
    and against the previous run;
  - the largest coefficient of variation (CV) seen WITHIN any one run, next to
    the changes BETWEEN runs. A within-run CV says how tightly repetitions
    inside one process agree. It says nothing about whether another session
    would agree, which is the question a before/after comparison depends on;
  - benchmarks split into single-threaded and two-threaded, because the two
    groups reproduce very differently.
"""
import json
import os
import re
import statistics as st
import sys

SUITES = ["bench_order_book", "bench_spsc_ring_buffer", "bench_matching_engine", "bench_baseline_mutex"]
# Benchmarks that start a second thread. Google Benchmark reports threads=1 for
# them (it does not see threads the benchmark creates), so go by name.
TWO_THREADED = ("ProducerConsumer", "Pipeline")


def load_run(path):
    run = {"dir": os.path.basename(os.path.normpath(path)), "median": {}, "cv": {}, "unit": {}}
    for suite in SUITES:
        with open(os.path.join(path, suite + ".json")) as f:
            for b in json.load(f)["benchmarks"]:
                if b["run_type"] != "aggregate":
                    continue
                key = (suite, b["run_name"])
                if b["aggregate_name"] == "median":
                    run["median"][key] = b["real_time"]
                    run["unit"][key] = b["time_unit"]
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
    for name, log in (("SPSC pipeline", "bench_matching_engine.log"), ("Mutex baseline", "bench_baseline_mutex.log")):
        with open(os.path.join(path, log)) as f:
            text = f.read()
        reports = re.findall(r"=== Latency Report \(\d+ samples\) ===\n(.*?)(?:\n\n|\Z)", text, re.S)
        pct = {}
        for body in reports:
            for k, v in re.findall(r"^(P50|P99)\s*:\s*(\d+) cycles", body, re.M):
                pct.setdefault(k, []).append(int(v))
        run["latency"][name] = pct
    return run


def pct_change(new, old):
    return (new / old - 1.0) * 100.0


def fmt_pct(value):
    """Signed percentage; a change that rounds to zero is shown without a sign."""
    return "0.0%" if abs(value) < 0.05 else f"{value:+.1f}%"


def fmt_time(value):
    return f"{value:,.1f}" if value < 100000 else f"{value:,.0f}"


def main(paths):
    runs = [load_run(p) for p in paths]
    labels = [chr(ord("A") + i) for i in range(len(runs))]
    out = []

    out += ["## Runs", "", "| Run | Directory | Recorded | Commit | Kernel | Cores | Verdict | Throttle delta |",
            "|---|---|---|---|---|---|---|---|"]
    for lab, r in zip(labels, runs):
        out.append(f"| {lab} | `{r['dir']}` | {r['when']} | `{r['commit']}` | {r['kernel']} | {r['cores']} | {r['verdict']} | {r['throttle']} |")
    out.append("")

    keys = [k for k in runs[0]["median"] if all(k in r["median"] for r in runs)]
    groups = {"Single-threaded": [k for k in keys if not any(t in k[1] for t in TWO_THREADED)],
              "Two-threaded": [k for k in keys if any(t in k[1] for t in TWO_THREADED)]}

    summary = []
    for title, group in groups.items():
        head = "| Benchmark | Unit | " + " | ".join(
            [f"{labels[0]} median"] + [f"{lab} median | {lab} vs {labels[0]}" + (f" | {lab} vs {labels[i]}" if i >= 1 else "")
                                       for i, lab in enumerate(labels[1:])]) + " | Largest within-run CV |"
        out += [f"## {title} benchmarks", "", head, "|---|---|" + "---:|" * (head.count("|") - 3)]
        vs_ref = {lab: [] for lab in labels[1:]}
        vs_prev = {lab: [] for lab in labels[2:]}
        for key in group:
            med = [r["median"][key] for r in runs]
            cells = [fmt_time(med[0])]
            for i in range(1, len(runs)):
                d_ref = pct_change(med[i], med[0])
                cells += [fmt_time(med[i]), fmt_pct(d_ref)]
                vs_ref[labels[i]].append(abs(d_ref))
                if i >= 2:
                    d_prev = pct_change(med[i], med[i - 1])
                    cells.append(fmt_pct(d_prev))
                    vs_prev[labels[i]].append(abs(d_prev))
            cv = max(r["cv"].get(key, float("nan")) for r in runs)
            out.append(f"| `{key[1]}` | {runs[0]['unit'][key]} | " + " | ".join(cells) + f" | {cv:.2f}% |")
        out.append("")
        for lab, vals in vs_ref.items():
            summary.append((title, f"{lab} vs {labels[0]}", st.median(vals), max(vals), len(vals)))
        for lab, vals in vs_prev.items():
            prev = labels[labels.index(lab) - 1]
            summary.append((title, f"{lab} vs {prev}", st.median(vals), max(vals), len(vals)))

    out += ["## Size of the change between runs", "",
            "| Group | Comparison | Benchmarks | Median absolute change | Largest absolute change |", "|---|---|---:|---:|---:|"]
    for title, comp, med, mx, n in summary:
        out.append(f"| {title} | {comp} | {n} | {med:.2f}% | {mx:.2f}% |")
    out.append("")

    def med_of(run, suite, name):
        return run["median"].get((suite, name))
    out += ["## Pipeline throughput at 100,000 orders (from real time)", "",
            "| Run | SPSC pipeline | Mutex baseline | SPSC / mutex |", "|---|---:|---:|---:|"]
    for lab, r in zip(labels, runs):
        a = med_of(r, "bench_matching_engine", "BM_Pipeline_Throughput/100000/iterations:3")
        b = med_of(r, "bench_baseline_mutex", "BM_MutexPipeline_Throughput/100000/iterations:3")
        if a and b:
            out.append(f"| {lab} | {100000 / (a / 1e3) / 1e6:.2f} M orders/s | {100000 / (b / 1e3) / 1e6:.2f} M orders/s | {b / a:.2f}× |")
    out.append("")

    out += ["## End-to-end latency at 100,000 orders (TSC ticks; median of the run's reports)", "",
            "| Run | SPSC P50 | SPSC P99 | Mutex P50 | Mutex P50 range | Mutex P99 |", "|---|---:|---:|---:|---:|---:|"]
    for lab, r in zip(labels, runs):
        s, m = r["latency"]["SPSC pipeline"], r["latency"]["Mutex baseline"]
        out.append(f"| {lab} | {st.median(s['P50']):,.0f} | {st.median(s['P99']):,.0f} | {st.median(m['P50']):,.0f} | "
                   f"{min(m['P50']):,}–{max(m['P50']):,} | {st.median(m['P99']):,.0f} |")
    out.append("")
    print("\n".join(out))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        sys.exit(1)
    main(sys.argv[1:])

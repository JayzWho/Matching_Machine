#!/usr/bin/env python3
"""Tables of the validation recording of the two-thread benchmark fixes.

Usage: scripts/diagnostics/analyze_two_thread_semantics.py STUDY_DIR > tables.md

Conditions (scripts/diagnostics/two_thread_semantics_matrix.json): queues,
spsc-pipeline and mutex-pipeline run the three two-thread programs of the
branch; spsc-pipeline-master and mutex-pipeline-master run the two pipelines
of master. A launch's value of a benchmark is the median of its counted
repetitions (all but the warm-up), and of a latency percentile the median of
its counted reports. The producer-to-consumer instances are summarized with
the functions of scripts/report/bench_report.py. Throughput differences are
differences of the medians of launch medians; p is the share of 20,000
shufflings of the launch medians between the two conditions that give at
least that difference (fixed seed).
"""
import csv
import json
import random
import statistics as st
import sys
from collections import defaultdict
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "report"))
import bench_report as report  # noqa: E402

TAIL = 4_000_000          # TSC ticks; the mark of an SPSC run whose two workers started on one CPU
RING = 4095               # orders ahead of an order that enters the pipeline's full ring
SEED, SHUFFLES = 20261028, 20000
PAIRS = (("spsc-pipeline-master", "spsc-pipeline", "BM_Pipeline_Throughput"),
         ("mutex-pipeline-master", "mutex-pipeline", "BM_MutexPipeline_Throughput"))
# The SPSC pipeline's latency-report runs and the quantity that is almost all queueing in each.
PER_ORDER = (("spsc-pipeline-master", "end-to-end"), ("spsc-pipeline", "queue"))


def read(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def table(header, rows):
    return ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)] + \
           ["| " + " | ".join(map(str, row)) + " |" for row in rows] + [""]


def pct(value):
    return f"{value:.1f}%"


def verdict(ok):
    return "**holds**" if ok else "**does not hold**"


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
    design = env["design"]
    launches = read(study / "launches.csv")
    out = [f"Study `{study.name}`: {design['sessions']} sessions × {design['blocks_per_session']} blocks, "
           f"{design['repetitions']} repetitions per launch, the first {design['warmup_repetitions']} a warm-up.", ""]

    # ── validity ──
    rows = []
    for condition in dict.fromkeys(row["condition"] for row in launches):
        mine = [row for row in launches if row["condition"] == condition]
        foreign = [float(row["foreign_ms"]) / float(row["wall_s"]) for row in mine]
        rows.append([condition, len(mine), sum(int(r["throttle_core"]) + int(r["throttle_package"]) for r in mine),
                     f"{sum(float(r['suspended_s']) for r in mine):.0f}", sum(int(r["device_irqs"]) for r in mine),
                     f"{st.median(foreign):.2f} / {max(foreign):.2f}",
                     f"{st.median(float(r['wall_s']) for r in mine):.1f}"])
    out += ["## Validity", ""]
    out += table(["Condition", "Launches", "Thermal throttle events", "Seconds suspended", "Device interrupts",
                  "Foreign CPU ms per s, median / max", "Wall s per launch, median"], rows)
    out += [f"Unmovable tasks allowed per session: {[s.get('unmovable_tasks') for s in env['sessions']]}.", ""]

    # ── repetitions ──
    reps = defaultdict(lambda: defaultdict(list))   # (condition, benchmark) -> launch -> [ns]
    for row in report.counted(read(study / "samples.csv")):
        reps[(row["condition"], row["benchmark"])][(int(row["session"]), int(row["block"]))].append(
            float(row["real_time_ns"]))

    # ── producer to consumer over a queue ──
    instances = defaultdict(lambda: defaultdict(dict))   # (benchmark, items) -> launch -> (region, placement) -> ns/item
    for (condition, name), by_launch in reps.items():
        found = report.sampled_instance(name) if condition == "queues" else None
        if found:
            benchmark, items, region, placement = found
            for launch, values in by_launch.items():
                instances[(benchmark, items)][launch][(region, placement)] = report.median(values) / items
    rows, verdicts = [], []
    for (benchmark, items) in sorted(k for k in instances if k[0] == report.TIERED):
        s = report.tier_summary(instances[(benchmark, items)])
        slow_counts = [sum(v > s["threshold"] for (_, p), v in cells.items() if p == placement)
                       for cells in instances[(benchmark, items)].values() for placement in (0, 1)]
        rows.append([f"{items:,}", f"{s['threshold']:.1f}", f"{s['gap']:.2f}×",
                     f"{report.median(s['fast']):.2f}", pct(report.cv(s["fast"])), pct(report.spread(s["fast"])),
                     f"{report.median(s['slow']):.1f}", pct(report.cv(s["slow"])), pct(report.spread(s["slow"])),
                     f"{s['share'][0]:.3f}", f"{s['share'][1]:.3f}", f"{min(slow_counts)}–{max(slow_counts)}",
                     s["both"]])
        verdicts.append((items, s))
    out += ["## int64 ring: two tiers", "",
            "ns per item. Threshold and gap from all instances of the recording (Otsu's method on the "
            "logarithms). A launch's level of a tier is the median of its instances in that tier; CV and "
            "spread are over the 16 launch levels. Slow regions per launch counts each placement separately "
            "(16 regions each). Both: launches with a region slow in both placements.", ""]
    out += table(["Items", "Threshold", "Gap", "Fast tier", "CV", "Spread", "Slow tier", "CV", "Spread",
                  "Slow share, placement 0", "placement 1", "Slow regions per launch and placement", "Both"], rows)
    for items, s in verdicts:
        h1 = s["gap"] >= 1.5 and (items != 100_000 or max(report.cv(s["fast"]), report.cv(s["slow"])) <= 3.0)
        h2 = all(0.15 <= share <= 0.35 for share in s["share"].values()) and s["both"] == 0
        out += [f"- {items:,} items: H1 (two tiers with a gap of at least 1.5×"
                + ("; each tier's launch levels within a CV of 3%" if items == 100_000 else "")
                + f") {verdict(h1)}; H2 (slow share 0.15–0.35 in each placement, no region slow in both) "
                + verdict(h2) + "."]
    out += [""]

    rows, h3 = [], []
    for (benchmark, items) in sorted(k for k in instances if k[0] != report.TIERED):
        meds, q1s, q3s = report.spread_summary(instances[(benchmark, items)])
        rows.append([f"`{benchmark}`", f"{items:,}", f"{report.median(meds):.1f}", pct(report.cv(meds)),
                     pct(report.spread(meds)), f"{min(meds):.1f} to {max(meds):.1f}",
                     f"{report.median(q1s):.1f} to {report.median(q3s):.1f}"])
        h3.append(report.cv(meds) <= 3.0)
    out += ["## Order ring and mutex queue: median over the 32 instances", "",
            "ns per item. A launch's median over its 32 instances; median, CV and spread over the 16 launches.", ""]
    out += table(["Benchmark", "Items", "Median", "CV", "Spread", "Lowest to highest launch",
                  "Quartiles over the instances, median over launches"], rows)
    out += [f"- H3 (CV of the launch medians at most 3%): {verdict(all(h3))}.", ""]

    # ── pipeline throughput against master ──
    rng, rows, h5 = random.Random(SEED), [], []
    for master, branch, family in PAIRS:
        for name in sorted({n for c, n in reps if c == branch and n.startswith(family + "/")},
                           key=lambda n: int(n.split("/")[1])):
            a = [report.median(v) / 1e6 for v in reps[(master, name)].values()]
            b = [report.median(v) / 1e6 for v in reps[(branch, name)].values()]
            change = (st.median(b) / st.median(a) - 1) * 100
            rows.append([f"`{report.base_name(name)}`", f"{st.median(a):.3f}", pct(report.cv(a)),
                         f"{st.median(b):.3f}", pct(report.cv(b)), f"{change:+.1f}%", f"{shuffle_p(a, b, rng):.3f}"])
            h5.append(change <= 2.0)
    out += ["## Pipeline throughput against master, ms per run", ""]
    out += table(["Benchmark", "Master", "CV", "Branch", "CV", "Change", "p"], rows)
    out += [f"- H5 (no pipeline more than 2% slower than master): {verdict(all(h5))}.", ""]

    # ── latency ──
    values = defaultdict(lambda: defaultdict(list))  # (condition, quantity, percentile) -> launch -> [ticks]
    for row in report.counted(read(study / "latency.csv")):
        values[(row["condition"], row["quantity"], row["percentile"])][
            (int(row["session"]), int(row["block"]))].append(int(row["tsc_ticks"]))
    rows, h4, medians = [], [], {}
    for (condition, quantity, percentile), by_launch in sorted(values.items()):
        if percentile not in ("P50", "P99", "Max"):
            continue
        meds = [report.median(v) for v in by_launch.values()]
        medians[(condition, quantity, percentile)] = report.median(meds)
        rows.append([condition, quantity, percentile, f"{report.median(meds):,.0f}", pct(report.cv(meds)),
                     f"{min(meds):,.0f} to {max(meds):,.0f}"])
        if quantity == "service" and percentile == "P50":
            h4.append(report.cv(meds) <= 3.0)
    out += ["## Latency at 100,000 orders, TSC ticks", "",
            "Median over launches of a launch's median report (5 counted reports per launch). The master "
            "pipelines report end-to-end latency.", ""]
    out += table(["Condition", "Quantity", "Percentile", "Median", "CV", "Lowest to highest launch"], rows)
    out += [f"- H4 (CV of the service-time P50 at most 3% in each pipeline): {verdict(all(h4))}.", ""]
    for condition, quantity in PER_ORDER:
        maxima = [x for x in values[(condition, quantity, "Max")].values() for x in x]
        out += [f"- `{condition}` {quantity}: {sum(x > TAIL for x in maxima)} of {len(maxima)} counted reports "
                f"have a maximum above {TAIL:,} ticks; the highest maximum is "
                f"{max(maxima) / report.median(maxima):.2f} times the median maximum."]
    out += [""]

    # ── how much the timestamps slow the SPSC consumer ──
    ghz = report.median([s[w]["tsc"]["calibration"]["ghz"] for s in env["sessions"] for w in ("before", "after")])
    rows = []
    for condition, quantity in PER_ORDER:
        latency_run = medians[(condition, quantity, "P50")] / RING / ghz
        throughput = report.median([report.median(v) for v in reps[(condition, next(
            n for c, n in reps if c == condition and n.startswith("BM_Pipeline_Throughput/100000/")))].values()]) / 1e5
        rows.append([condition, f"{latency_run:.0f}", f"{throughput:.0f}", f"{latency_run / throughput:.2f}"])
    out += ["## SPSC consumer time per order, latency-report run against throughput run", "",
            f"In the full ring an order waits for the {RING:,} ahead of it, so the median queueing time divided "
            f"by {RING:,} estimates the consumer's time per order in the latency-report run (ns, at the "
            "calibrated TSC frequency). On master the end-to-end latency, almost all queueing, checks the "
            "estimate. The throughput run's time is the 100,000-order run over 100,000.", ""]
    out += table(["Condition", "Latency-report run, ns per order", "Throughput run, ns per order", "Ratio"], rows)

    # ── calibration ──
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

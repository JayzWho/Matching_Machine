#!/usr/bin/env python3
"""Turn a study recorded by bench_record.py into Markdown tables.

Usage:
    scripts/report/bench_report.py STUDY_DIR [--permutations N]  > tables.md

Everything is computed from the CSV files in STUDY_DIR and from env.json, with
the standard library only and a fixed random seed, so the output can be
regenerated and compared byte for byte.

The question the tables answer is whether results move between launches of
unchanged binaries by more than the repetitions inside a launch scatter. For
every benchmark the tables give:

  middle half of the launch medians
      the distance between the first and third quartile of the launch medians,
      relative to their median: how far a typical launch is from another;
  spread of the launch medians
      the largest launch median relative to the smallest, minus one: how far
      the two most distant launches are from each other;
  spread expected from repetition scatter
      what that spread would be if only the scatter inside a launch existed:
      every repetition is divided by its launch median, which removes any
      difference between launches, the results are shuffled between launches
      many times, and the 95th percentile of the resulting spreads is taken;
  p
      the repetitions are ranked and shuffled between launches the same number
      of times; p is the share of shuffles in which the launches differ from
      each other at least as much as observed, measured by the Kruskal-Wallis
      statistic (the variance of the launches' rank sums). It is small when
      repetitions of one launch resemble each other more than they resemble
      those of other launches.

The statistic is not the spread itself because the spread cannot detect a
benchmark that settles on one of two levels at each launch: shuffling such
repetitions still leaves some launches at each level, so the shuffled spread
equals the observed one.

A benchmark is marked as moving between launches when p is below 0.001, which
keeps the chance of a false mark near 5% across all benchmarks of a study.

The producer-to-consumer benchmarks of bench_spsc_ring_buffer run every
instance on one of 16 memory regions in each of two thread placements, and get
a section of their own: the int64 ring by the level of each of its two states
and the share of slow instances, the Order ring and the mutex queue by the
median over a launch's instances.
"""
import argparse
import csv
import json
import math
import random
import re
import statistics as st
import sys
from collections import defaultdict
from pathlib import Path

SEED = 20261012
ALPHA = 0.001
NOT_TIMED = "_LatencyReport"    # their timed region is the printing of the latency report
# (title, threads in the timed region, predicate on (binary, benchmark)).
FAMILIES = [
    ("Order book and matching", 1,
     lambda b, n: b == "bench_order_book" or n.startswith("BM_SingleThread_Baseline")),
    ("Ring buffer push and pop", 1,
     lambda b, n: n.startswith("BM_SPSC_SingleThread_")),
    ("SPSC ring buffer, producer to consumer", 2,
     lambda b, n: n.startswith(("BM_SPSC_ProducerConsumer", "BM_SPSC_Order_ProducerConsumer"))),
    ("Mutex queue, producer to consumer", 2,
     lambda b, n: n.startswith("BM_Mutex_ProducerConsumer")),
    ("SPSC pipeline", 2,
     lambda b, n: n.startswith("BM_Pipeline_Throughput")),
    ("Mutex pipeline", 2,
     lambda b, n: n.startswith("BM_MutexPipeline_Throughput")),
]
PIPELINES = {"bench_matching_engine": "SPSC pipeline", "bench_baseline_mutex": "Mutex pipeline"}
# Names as recorded before the pipeline throughput benchmarks reported real
# time; since then Google Benchmark appends "/real_time" to them (base_name()).
RATIO = (("bench_matching_engine", "BM_Pipeline_Throughput/100000/iterations:3"),
         ("bench_baseline_mutex", "BM_MutexPipeline_Throughput/100000/iterations:3"))
# The benchmarks whose launch medians are listed round by round.
ROUND_BY_ROUND = ("BM_SPSC_ProducerConsumer/100000/real_time", "BM_Mutex_ProducerConsumer/100000/real_time",
                  "BM_Pipeline_Throughput/100000/iterations:3", "BM_MutexPipeline_Throughput/100000/iterations:3")
# Producer-to-consumer instances: BM_<name>/<items>/block:<region>/placement:<0 or 1>/...
SAMPLED = re.compile(r"^(BM_\w+)/(\d+)/block:(\d+)/placement:(\d+)/")
# The benchmark whose instances settle in one of two states, reported by tier.
TIERED = "BM_SPSC_ProducerConsumer"
FOREIGN_LIMIT = 50.0    # ms of foreign CPU time per second above which a launch counts as disturbed
STRONG = 0.7            # |Spearman rho| from which a telemetry series is listed
# Launch telemetry tested against the launch medians, as (label, value of a launches.csv row).
PER_SECOND = ("foreign_ms", "waited_ms", "involuntary_switches", "voluntary_switches", "irq_LOC", "irq_RES",
              "softirq_TIMER", "softirq_SCHED", "softirq_RCU")
PLAIN = ("temp_start_c", "temp_end_c", "position", "cpu_per_wall")


def read(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


def base_name(name):
    """A pipeline throughput benchmark's name without the suffix that real time added."""
    stem = name.removesuffix("/real_time")
    return stem if stem.startswith(("BM_Pipeline_Throughput/", "BM_MutexPipeline_Throughput/")) else name


def unit_note(env):
    """What the study knows about the counter's frequency."""
    found = [s[when].get("tsc", {}).get("calibration") or {} for s in env["sessions"] for when in ("before", "after")
             if when in s]
    ghz = [c["ghz"] for c in found if c.get("ghz")]
    if not ghz:
        return "not converted to time because the counter's frequency has not been calibrated."
    return (f"not converted to time. Calibrated against CLOCK_MONOTONIC before and after each session, the "
            f"counter ran at {min(ghz):.6f} to {max(ghz):.6f} GHz ({len(ghz)} calibrations, env.json).")


def sampled_instance(name):
    """(benchmark, items, region, placement) of a producer-to-consumer instance, else None."""
    m = SAMPLED.match(name)
    return (m[1], int(m[2]), int(m[3]), int(m[4])) if m else None


def tier_threshold(values):
    """Where per-item times split into a fast and a slow tier: Otsu's method on
    their logarithms, i.e. the split that maximizes the variance between the
    two groups, which a single outlier hardly moves. Returns (threshold, gap),
    the gap being the fastest slow value divided by the slowest fast one, or
    None when all values are equal."""
    logs = sorted(math.log(v) for v in values)
    total, left, best, at = sum(logs), 0.0, -1.0, None
    for i in range(1, len(logs)):
        left += logs[i - 1]
        if logs[i] == logs[i - 1]:
            continue
        w0, w1 = i / len(logs), (len(logs) - i) / len(logs)
        between = w0 * w1 * (left / i - (total - left) / (len(logs) - i)) ** 2
        if between > best:
            best, at = between, i
    if at is None:
        return None
    low, high = math.exp(logs[at - 1]), math.exp(logs[at])
    return math.sqrt(low * high), high / low


def tier_summary(launches):
    """The two tiers of the int64 ring. `launches` maps a launch to
    {(region, placement): ns per item}. Returns None when the values do not
    split, else a dict: threshold and gap; per tier the launch levels (median
    of a launch's instances in that tier); the share of slow instances by
    placement over all launches; and the launches with a region slow in both
    placements."""
    split = tier_threshold([v for cells in launches.values() for v in cells.values()])
    if split is None:
        return None
    threshold, gap = split
    fast, slow, share, both = [], [], defaultdict(list), 0
    for cells in launches.values():
        tier = {cell: v > threshold for cell, v in cells.items()}
        fast += [median([v for cell, v in cells.items() if not tier[cell]])] if not all(tier.values()) else []
        slow += [median([v for cell, v in cells.items() if tier[cell]])] if any(tier.values()) else []
        for (region, placement), is_slow in tier.items():
            share[placement].append(is_slow)
        both += any(tier.get((region, 0)) and tier.get((region, 1)) for region, _ in cells)
    return {"threshold": threshold, "gap": gap, "fast": fast, "slow": slow,
            "share": {p: sum(v) / len(v) for p, v in sorted(share.items())}, "both": both}


def spread_summary(launches):
    """The Order ring and the mutex queue: per launch the median and the
    quartiles of its instances' ns per item, as three lists over launches."""
    meds, q1s, q3s = [], [], []
    for cells in launches.values():
        values = list(cells.values())
        q1, _, q3 = st.quantiles(values, n=4, method="inclusive")
        meds.append(median(values))
        q1s.append(q1)
        q3s.append(q3)
    return meds, q1s, q3s


def latency_groups(rows):
    """P50 and P99 of the counted latency reports, as
    {(condition, pipeline, quantity, percentile): {(session, round): [TSC ticks]}}.
    Studies recorded before the split into queue time and service time have
    no `quantity` column; their reports are the end-to-end latency."""
    groups = defaultdict(lambda: defaultdict(list))
    for row in counted(rows):
        if row["percentile"] in ("P50", "P99"):
            key = (row["condition"], PIPELINES[row["binary"]], row.get("quantity") or "end-to-end", row["percentile"])
            groups[key][(int(row["session"]), int(row["round"]))].append(int(row["tsc_ticks"]))
    return groups


def counted(rows):
    """The rows not marked as a warm-up. Studies recorded before the mark
    existed have no `warmup` column, and all of their rows count."""
    return [row for row in rows if not int(row.get("warmup") or 0)]


def median(values):
    s = sorted(values)
    k = len(s) // 2
    return s[k] if len(s) % 2 else (s[k - 1] + s[k]) / 2


def spread(values):
    return (max(values) / min(values) - 1.0) * 100.0


def cv(values):
    return st.stdev(values) / st.mean(values) * 100.0 if len(values) > 1 else float("nan")


def p_value(null, observed):
    return (sum(1 for v in null if v >= observed) + 1) / (len(null) + 1)


def middle_half(values):
    """Distance between the quartiles, relative to the median, in percent."""
    q1, _, q3 = st.quantiles(values, n=4, method="inclusive")
    return (q3 - q1) / median(values) * 100.0


def rank_sum_test(rank_list, sizes, n, rng):
    """p for the groups (consecutive runs of the given sizes) differing, by the Kruskal-Wallis statistic."""
    def statistic(r):
        i, total = 0, 0.0
        for size in sizes:
            part = sum(r[i:i + size])
            total += part * part / size
            i += size
        return total
    r = list(rank_list)
    observed, null = statistic(r), []
    for _ in range(n):
        rng.shuffle(r)
        null.append(statistic(r))
    return p_value(null, observed)


def launch_test(groups, n, rng):
    """Spread of the launch medians, the spread expected from repetition scatter, and p."""
    meds, sizes = [median(g) for g in groups], [len(g) for g in groups]
    p = rank_sum_test(ranks([v for g in groups for v in g]), sizes, n, rng)
    flat, scatter = [v / m for g, m in zip(groups, meds) for v in g], []
    for _ in range(n):
        rng.shuffle(flat)
        i, shuffled = 0, []
        for size in sizes:
            shuffled.append(median(flat[i:i + size]))
            i += size
        scatter.append(spread(shuffled))
    scatter.sort()
    return spread(meds), scatter[int(0.95 * (len(scatter) - 1))], p


def group_test(values, labels, n, rng):
    """Spread of the group medians (groups given by labels) and p for the groups differing."""
    groups = defaultdict(list)
    for v, r, g in zip(values, ranks(values), labels):
        groups[g].append((v, r))
    members = list(groups.values())
    p = rank_sum_test([r for m in members for _, r in m], [len(m) for m in members], n, rng)
    return spread([median([v for v, _ in m]) for m in members]), p


def ranks(values):
    order = sorted(range(len(values)), key=values.__getitem__)
    out, i = [0.0] * len(values), 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            out[order[k]] = (i + j) / 2 + 1
        i = j + 1
    return out


def spearman(x, y):
    if len(x) < 3 or len(set(x)) < 2 or len(set(y)) < 2:
        return float("nan")
    return st.correlation(ranks(x), ranks(y))


def trend_test(values, n, rng):
    """Rank correlation of the launch medians with launch order, and its p."""
    order = list(range(len(values)))
    observed = spearman(order, values)
    shuffled, null = list(values), []
    for _ in range(n):
        rng.shuffle(shuffled)
        null.append(abs(spearman(order, shuffled)))
    return observed, p_value(null, abs(observed))


# ── formatting ───────────────────────────────────────────────────────────────

def fmt_sig(value):
    """Four significant figures, with thousands separators."""
    if value >= 1000:
        return f"{value:,.0f}"
    return f"{value:.{max(3 - len(str(int(value))) + 1, 0)}f}"


def unit_for(ns):
    for unit, div in (("ns", 1.0), ("us", 1e3), ("ms", 1e6)):
        if ns < div * 1e4 or unit == "ms":
            return unit, div


def fmt_time(ns):
    unit, div = unit_for(ns)
    return f"{fmt_sig(ns / div)} {unit}"


def fmt_p(p, n):
    return f"<{1 / (n + 1):.5f}".rstrip("0") if p <= 1 / (n + 1) else (f"{p:.4f}" if p < 0.01 else f"{p:.2f}")


def mark(p, n):
    return f"**{fmt_p(p, n)}**" if p < ALPHA else fmt_p(p, n)


def pct(value):
    return f"{value:.1f}%"


def span(values):
    return f"{min(values):.1f}% to {max(values):.1f}%"


def table(header, rows, right_from=1):
    out = ["| " + " | ".join(header) + " |",
           "|" + "".join("---|" if i < right_from else "---:|" for i in range(len(header)))]
    out += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return out + [""]


# ── report ───────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("study", type=Path)
    ap.add_argument("--permutations", type=int, default=20000)
    opts = ap.parse_args()
    n, rng = opts.permutations, random.Random(SEED)
    env = json.loads((opts.study / "env.json").read_text())
    launches = read(opts.study / "launches.csv")
    conditions = list(dict.fromkeys(l["condition"] for l in launches))

    # reps[condition][(binary, benchmark)][(session, round)] = [real ns per repetition]
    reps = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for row in counted(read(opts.study / "samples.csv")):
        reps[row["condition"]][(row["binary"], row["benchmark"])][(int(row["session"]), int(row["round"]))] \
            .append(float(row["real_time_ns"]))
    launch_row = {(l["condition"], l["binary"], int(l["session"]), int(l["round"])): l for l in launches}
    interval = {(r["condition"], r["binary"], r["benchmark"], int(r["session"]), int(r["round"])): r
                for r in read(opts.study / "intervals.csv")}

    keys = [k for k in next(iter(reps.values())) if NOT_TIMED not in k[1] and not sampled_instance(k[1])]
    families = [(title, threads, [k for k in keys if member(*k)]) for title, threads, member in FAMILIES]
    orphans = [k[1] for k in keys if not any(k in group for _, _, group in families)]
    if orphans:
        sys.exit("bench_report.py: benchmarks without a family: " + ", ".join(orphans))

    out = []

    # ── design ──
    d, first = env["design"], env["sessions"][0]
    per_condition = {c: sum(1 for l in launches if l["condition"] == c and l["binary"] == launches[0]["binary"])
                     for c in conditions}
    out += ["## Design", ""]
    out += table(["", ""], [
        ["Label", env["label"]],
        ["Commit", f"`{env['commit']}`" + (" with uncommitted changes to build inputs" if env["build_inputs_dirty"] else "")],
        ["Measurement mode", "yes" if env["measurement_mode"] else "NO — result is not valid"],
        ["Kernel", first["before"]["os"]["kernel"]],
        ["Measurement cores", ", ".join(map(str, first["cores"])) + f" (single-threaded binaries on {first['cores'][0]})"],
        ["Recorded", f"{launches[0]['started'][:16].replace('T', ' ')} to {launches[-1]['started'][11:16]}"],
        ["Sessions × rounds × repetitions", f"{d['sessions']} × {d['rounds_per_session']} × {d['repetitions']}"
         + (f", the first {d['warmup_repetitions']} of each benchmark a warm-up, left out"
            if d.get("warmup_repetitions") else "")],
        ["Launches of each binary", ", ".join(f"{v} {c}" for c, v in per_condition.items())],
        ["Binaries built", ", ".join(sorted({v[:10] for v in env["binaries_built"].values()}))],
    ], right_from=2)

    # ── validity and interference ──
    def rate(l, column):
        return float(l[column]) / float(l["wall_s"])
    rows = []
    for c in conditions:
        mine = [l for l in launches if l["condition"] == c]
        mhz = {width: [float(l[k]) for l in mine for k in ("mhz_min", "mhz_max")
                       if l[k] and len(l["cores"].split()) == width] for width in (1, 2)}
        temps = [float(l[k]) for l in mine for k in ("temp_start_c", "temp_end_c")]
        foreign = [rate(l, "foreign_ms") for l in mine]
        waited = [rate(l, "waited_ms") for l in mine]
        rows.append([c, len(mine),
                     sum(int(l["throttle_core"]) + int(l["throttle_package"]) for l in mine),
                     f"{sum(float(l['suspended_s']) for l in mine):.0f}",
                     sum(int(l["device_irqs"]) for l in mine),
                     sum(int(l["irq_TRM"]) for l in mine),
                     *(f"{min(mhz[w]):.0f} to {max(mhz[w]):.0f}" if mhz[w] else "n/a" for w in (1, 2)),
                     f"{min(temps):.0f} to {max(temps):.0f}",
                     f"{median(foreign):.2f} / {max(foreign):.2f}",
                     sum(1 for v in foreign if v > FOREIGN_LIMIT),
                     f"{median(waited):.2f} / {max(waited):.2f}",
                     f"{max(int(l['user_tasks_moved']) for l in mine)}"])
    out += ["## Validity and interference", ""]
    out += table(["Condition", "Launches", "Thermal throttle events", "Seconds suspended",
                  "Device interrupts", "Thermal interrupts", "MHz, launches on one core",
                  "MHz, launches on two cores", "Package °C", "Foreign CPU ms per s, median / max",
                  f"Launches above {FOREIGN_LIMIT:.0f} ms per s", "Run-queue wait ms per s, median / max",
                  "User tasks moved off the cores"], rows)
    out += ["Foreign CPU is time that tasks other than the benchmark ran on the launch's cores. A frequency "
            "reading means something only while the core is busy: a launch on one core keeps it busy throughout, "
            "while a launch on two cores leaves one idle during its single-threaded benchmarks, and the reading "
            "does not say which core it came from.", ""]

    # ── per benchmark ──
    result = {}     # (condition, key) -> dict
    for c in conditions:
        for key in keys:
            by_launch = reps[c][key]
            order = sorted(by_launch)
            groups = [by_launch[k] for k in order]
            meds = [median(g) for g in groups]
            observed, noise, p = launch_test(groups, n, rng)
            s_spread, s_p = group_test(meds, [k[0] for k in order], n, rng)
            rho, rho_p = trend_test(meds, min(n, 5000), rng)
            centre = median(meds)
            result[(c, key)] = {"order": order, "medians": meds, "median": centre,
                                "cv": median([cv(g) for g in groups]), "middle": middle_half(meds),
                                "spread": observed, "noise": noise, "p": p,
                                "beyond": sum(1 for m in meds if abs(m / centre - 1.0) * 100.0 > noise),
                                "session_spread": s_spread, "session_p": s_p, "rho": rho, "rho_p": rho_p}

    out += ["## Summary by family", "",
            "How far launches are from each other, as the range over the benchmarks of a family of two measures: "
            "the middle half of the launch medians (the distance between their quartiles, relative to their "
            "median) and their spread (the largest relative to the smallest, minus one). Within 1% counts the "
            "launch medians that are within 1% of the median of their benchmark. Moving counts the benchmarks "
            f"whose launches differ by more than their repetitions scatter (p < {ALPHA}).", ""]
    rows = []
    for title, threads, group in families:
        if not group:
            continue
        row = [title, threads]
        for c in conditions:
            mine = [result[(c, k)] for k in group]
            near = sum(1 for r in mine for m in r["medians"] if abs(m / r["median"] - 1.0) <= 0.01)
            row += [span([r["middle"] for r in mine]), span([r["spread"] for r in mine]),
                    f"{near} of {sum(len(r['medians']) for r in mine)}",
                    f"{sum(1 for r in mine if r['p'] < ALPHA)} of {len(mine)}"]
        rows.append(row)
    header = ["Family", "Threads"]
    for c in conditions:
        header += [f"Middle half, {c}", f"Spread, {c}", f"Within 1%, {c}", f"Moving, {c}"]
    out += table(header, rows)

    out += ["## Launch medians by benchmark", "",
            "The middle half is the distance between the quartiles of the launch medians, relative to their "
            "median. A launch is beyond scatter when its median is farther from the median of all launches than "
            "the spread expected from scatter. The three p columns test for differences between launches, "
            f"between sessions, and for a trend over the recording; values below {ALPHA} are bold.", ""]
    for c in conditions:
        out += [f"### Condition: {c}", ""]
        for title, threads, group in families:
            rows = []
            for key in group:
                r = result[(c, key)]
                rows.append([f"`{key[1]}`", fmt_time(r["median"]), pct(r["cv"]), pct(r["middle"]), pct(r["spread"]),
                             pct(r["noise"]), f"{r['beyond']} of {len(r['medians'])}", mark(r["p"], n),
                             mark(r["session_p"], n), mark(r["rho_p"], min(n, 5000))])
            if rows:
                out += [f"**{title}** ({'one thread' if threads == 1 else 'two threads'})", ""]
                out += table(["Benchmark", "Median", "CV inside a launch", "Middle half of launch medians",
                              "Spread of launch medians", "Expected from scatter", "Launches beyond scatter",
                              "p (launches)", "p (sessions)", "p (trend)"], rows)

    # ── producer to consumer over a queue ──
    # instances[(condition, benchmark, items)][launch][(region, placement)] = ns per item
    instances = defaultdict(lambda: defaultdict(dict))
    for c in conditions:
        for (binary, name), by_launch in reps[c].items():
            found = sampled_instance(name)
            if found:
                benchmark, items, region, placement = found
                for launch, values in by_launch.items():
                    instances[(c, benchmark, items)][launch][(region, placement)] = median(values) / items
    if instances:
        order = sorted(instances, key=lambda k: (conditions.index(k[0]), k[1], k[2]))
        out += ["## Producer to consumer over a queue", "",
                "Every instance runs on one of 16 memory regions in one of two thread placements (placement 0 puts "
                "the producer on the lower-numbered CPU), and is represented by the median of its repetitions, in "
                "ns per item. The int64 ring settles in one of two states, so its instances are split into a fast "
                "and a slow tier where the logarithms of all of the study's instances split best (Otsu's method); "
                "the gap is the fastest slow instance divided by the slowest fast one. A launch's level of a tier "
                "is the median of its instances in that tier. The CV is that of the launch levels or medians.", ""]
        rows = []
        for key in order:
            if key[1] != TIERED:
                continue
            s = tier_summary(instances[key])
            if s is None:
                continue
            rows.append([key[0], f"`{key[1]}`", f"{key[2]:,}", f"{s['threshold']:.1f}", f"{s['gap']:.2f}×",
                         f"{median(s['fast']):.2f}" if s["fast"] else "n/a", pct(cv(s["fast"])),
                         f"{median(s['slow']):.1f}" if s["slow"] else "n/a", pct(cv(s["slow"])),
                         *(f"{s['share'].get(p, float('nan')):.2f}" for p in (0, 1)),
                         f"{s['both']} of {len(instances[key])}"])
        out += table(["Condition", "Benchmark", "Items", "Threshold, ns", "Gap", "Fast tier, ns", "CV",
                      "Slow tier, ns", "CV", "Slow share, placement 0", "Slow share, placement 1",
                      "Launches with a region slow in both placements"], rows, right_from=2)
        rows = []
        for key in order:
            if key[1] == TIERED:
                continue
            meds, q1s, q3s = spread_summary(instances[key])
            rows.append([key[0], f"`{key[1]}`", f"{key[2]:,}", len(meds), f"{median(meds):.1f}", pct(cv(meds)),
                         pct(spread(meds)), f"{median(q1s):.1f} to {median(q3s):.1f}"])
        out += table(["Condition", "Benchmark", "Items", "Launches", "Median of launch medians, ns", "CV",
                      "Spread", "Quartiles over the instances, median over launches, ns"], rows, right_from=2)

    # ── round by round ──
    listed = [k for k in keys if base_name(k[1]) in ROUND_BY_ROUND]
    if listed:
        out += ["## Launch medians round by round", "",
                "One row per session, one column per round, in recording order. The letter after each value is "
                "the condition of that round: " + ", ".join(f"{c[0]} for {c}" for c in conditions) + ".", ""]
        for key in sorted(listed, key=lambda k: ROUND_BY_ROUND.index(base_name(k[1]))):
            cell = {launch: (m, c) for c in conditions
                    for launch, m in zip(result[(c, key)]["order"], result[(c, key)]["medians"])}
            unit, div = unit_for(median([m for m, _ in cell.values()]))
            sessions = sorted({s for s, _ in cell})
            rounds = sorted({rd for _, rd in cell})
            out += [f"`{key[1]}`, {unit}", ""]
            out += table(["Session"] + [f"Round {rd}" for rd in rounds],
                         [[s] + [f"{fmt_sig(cell[(s, rd)][0] / div)} {cell[(s, rd)][1][0]}" if (s, rd) in cell else ""
                                 for rd in rounds] for s in sessions])

    # ── conditions against each other ──
    if len(conditions) == 2:
        a, b = conditions
        rows = []
        for title, threads, group in families:
            for key in group:
                ra, rb = result[(a, key)], result[(b, key)]
                values = ra["medians"] + rb["medians"]
                labels = [a] * len(ra["medians"]) + [b] * len(rb["medians"])
                _, p = group_test(values, labels, n, rng)
                rows.append([f"`{key[1]}`", fmt_time(ra["median"]), fmt_time(rb["median"]),
                             f"{(rb['median'] / ra['median'] - 1) * 100:+.1f}%", mark(p, n),
                             pct(ra["spread"]), pct(rb["spread"])])
        out += [f"## {a} against {b}", "",
                f"Median of the launch medians under each condition, the change from {a} to {b}, and p for "
                "the two conditions differing, from shuffled condition labels.", ""]
        out += table(["Benchmark", f"Median, {a}", f"Median, {b}", "Change", "p",
                      f"Spread, {a}", f"Spread, {b}"], rows)

    # ── what goes with the movement ──
    share = {}

    def chance(count):
        """Share of unrelated series of this length whose |rho| reaches STRONG."""
        if count not in share:
            order, shuffled, hits = list(range(count)), list(range(count)), 0
            for _ in range(20000):
                rng.shuffle(shuffled)
                hits += abs(spearman(order, shuffled)) >= STRONG
            share[count] = hits / 20000
        return share[count]

    rows, moving, tested, by_chance = [], 0, 0, 0.0
    for c in conditions:
        for key in keys:
            r = result[(c, key)]
            if r["p"] >= ALPHA:
                continue
            binary, name = key
            mine = [launch_row[(c, binary, s, rd)] for s, rd in r["order"]]
            mine_iv = [interval.get((c, binary, name, s, rd)) for s, rd in r["order"]]
            series = {f"{col} per s": [rate(l, col) for l in mine] for col in PER_SECOND}
            series.update({col: [float(l[col]) for l in mine] for col in PLAIN})
            cores = sorted(k for k in mine[0] if k.startswith("ran_s_cpu"))
            if len(cores) == 2:
                series[f"share of CPU time on {cores[0][6:]}"] = [
                    float(l[cores[0]]) / max(float(l[cores[0]]) + float(l[cores[1]]), 1e-9) for l in mine]
            if all(iv and iv["cpu_per_wall"] for iv in mine_iv):
                series["benchmark: CPU time per wall time"] = [float(iv["cpu_per_wall"]) for iv in mine_iv]
                series["benchmark: run-queue wait ms per s"] = [
                    float(iv["waited_ms"]) / float(iv["wall_s"]) for iv in mine_iv]
            found = sorted(((spearman(v, r["medians"]), label) for label, v in series.items()),
                           key=lambda t: -abs(t[0]) if t[0] == t[0] else 0)
            listed = [f"{label} ({rho:+.2f})" for rho, label in found if rho == rho and abs(rho) >= STRONG]
            moving += 1
            tested += sum(1 for rho, _ in found if rho == rho)
            by_chance += sum(1 for rho, _ in found if rho == rho) * chance(len(r["medians"]))
            if listed:
                rows.append([c, f"`{name}`", pct(r["spread"]), "; ".join(listed)])
    out += ["## What accompanies the movement", "",
            "The rank correlation (Spearman ρ) of a benchmark's launch medians with what was recorded around "
            f"each launch, for the benchmarks that move between launches. Only |ρ| ≥ {STRONG} is listed. "
            "`benchmark:` values cover the benchmark's own part of the launch; the others cover the whole launch "
            f"of its binary. {tested} pairs of benchmark and recorded series were tested, and about "
            f"{by_chance:.0f} of them would reach that correlation without any relation. {moving - len(rows)} of "
            f"the {moving} moving benchmarks have no such correlate and are not listed. A correlation does not "
            "say which of the two is the cause.", ""]
    out += table(["Condition", "Benchmark", "Spread of launch medians", "Correlates"], rows, right_from=2) \
        if rows else []

    # ── two-thread scheduling ──
    out += ["## How the two-thread benchmarks were scheduled", "",
            "CPU time per wall time is 2 when both threads run all the time on separate cores and 1 when they "
            "share one core or one of them always waits. Run-queue wait is time a runnable task spent waiting "
            "for one of the two cores.", ""]
    rows = []
    for c in conditions:
        for title, threads, group in families:
            if threads != 2:
                continue
            for key in group:
                ivs = [interval.get((c, key[0], key[1], s, rd)) for s, rd in result[(c, key)]["order"]]
                ivs = [iv for iv in ivs if iv and iv["cpu_per_wall"]]
                if not ivs:
                    continue
                par = [float(iv["cpu_per_wall"]) for iv in ivs]
                wait = [float(iv["waited_ms"]) / float(iv["wall_s"]) for iv in ivs]
                rows.append([c, f"`{key[1]}`", f"{min(par):.2f}", f"{median(par):.2f}", f"{max(par):.2f}",
                             f"{median(wait):.1f}", f"{max(wait):.1f}"])
    out += table(["Condition", "Benchmark", "CPU per wall, min", "median", "max",
                  "Run-queue wait ms per s, median", "max"], rows, right_from=2)

    # ── pipeline ratio and latency ──
    rows = []
    for c in conditions:
        found = {(binary, base_name(name)): launches_of for (binary, name), launches_of in reps[c].items()}
        spsc, mutex = found.get(RATIO[0]), found.get(RATIO[1])
        if spsc and mutex:
            ratios = [median(mutex[k]) / median(spsc[k]) for k in sorted(spsc) if k in mutex]
            rows.append([c, len(ratios), f"{min(ratios):.2f}×", f"{median(ratios):.2f}×", f"{max(ratios):.2f}×"])
    out += ["## Pipeline throughput ratio at 100,000 orders", "",
            "SPSC pipeline throughput divided by mutex pipeline throughput, one value per round from the two "
            "launches of that round.", ""]
    out += table(["Condition", "Rounds", "Lowest", "Median", "Highest"], rows)

    latency = latency_groups(read(opts.study / "latency.csv"))
    rows = []
    for (c, pipeline, quantity, percentile), by_launch in sorted(
            latency.items(), key=lambda item: (conditions.index(item[0][0]), item[0][1:])):
        meds = [median(v) for _, v in sorted(by_launch.items())]
        inside = median([max(v) / min(v) for v in by_launch.values()])
        rows.append([c, pipeline, quantity, percentile, f"{median(meds):,.0f}", f"{min(meds):,.0f}",
                     f"{max(meds):,.0f}", pct(spread(meds)), f"{inside:.1f}×"])
    out += ["## Pipeline latency at 100,000 orders", "",
            "Queue time runs from the producer's timestamp to the moment the consumer takes the order out of the "
            "queue, service time from then until matching is done; studies recorded before the two were split "
            "report their sum, end-to-end. In TSC ticks, the unit of the processor's timestamp counter; "
            + unit_note(env) + " Each launch prints one report of each quantity per repetition; a launch is "
            "represented by the median of its reports" + (", leaving out the warm-up's." if d.get("warmup_repetitions")
                                                           else "."), ""]
    out += table(["Condition", "Pipeline", "Quantity", "Percentile", "Median of launches", "Lowest launch",
                  "Highest launch", "Spread of launch medians", "Highest / lowest report inside a launch, median"],
                 rows, right_from=4)

    print("\n".join(out))


if __name__ == "__main__":
    main()

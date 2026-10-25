#!/usr/bin/env python3
"""Tables of the queue-regimes study.

Usage: python3 scripts/diagnostics/analyze_queue_regimes.py STUDY

A region is one memory region of one launch. An orientation is the CPU the
producer ran on; the consumer ran on the other one. A cell is one region in
one orientation, and its value is the median of its repetitions: nanoseconds
per item for a queue benchmark, with the retry counters per item. A
repetition whose iterations did not all start the producer on the same CPU
belongs to no cell. A round trip is the median over the region's repetitions
of BM_LineRoundTrip, in nanoseconds. The raw JSON files are read from
raw.tar.gz without extracting it.

The int64 ring is slow above SLOW_NS per item. The consumer has caught up when
the queue holds less than 1% of the items as the producer finishes and the
consumer waited before at least 1% of them; it has not when the queue holds at
least 10% and it waited before less than 0.1%.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import statistics as stats
import tarfile

SLOW_NS = 20.0
ROUNDS = 20_000
FAMILIES = {
    "BM_SPSC_ProducerConsumer": ("int64, checksum stored", 0),
    "BM_SPSC_Local_ProducerConsumer": ("int64, local checksum", 0),
    "BM_SPSCCached_ProducerConsumer": ("int64, cached ring", 0),
    "BM_SPSC_Order_ProducerConsumer": ("Order", 1),
    "BM_SPSCCached_Order_ProducerConsumer": ("Order, cached ring", 1),
    "BM_Mutex_ProducerConsumer": ("mutex", 3),
}
ORIGINAL_INT64 = ("BM_SPSC_ProducerConsumer", "BM_SPSC_Local_ProducerConsumer")
COUNTERS = ("pop_fails", "pop_waits", "depth_end", "push_fails", "same_cpu")


def load(study):
    """Cells keyed by (family, launch, index, producer CPU), each a list of
    repetitions; the repetitions of each round trip keyed by (launch, kind,
    index, mode); and the number of repetitions that belong to no cell."""
    cells, trips, unplaced = defaultdict(list), defaultdict(list), 0
    with tarfile.open(study / "raw.tar.gz", "r:gz") as archive:
        members = {Path(m.name).name: m for m in archive.getmembers() if m.isfile()}
        for name, member in sorted(members.items()):
            if not re.fullmatch(r"s\d+_b\d+_c\d+\.command\.json", name):
                continue
            launch = name.split(".")[0]
            if not json.load(archive.extractfile(member)).get("complete"):
                continue
            for run in json.load(archive.extractfile(members[f"{launch}.json"]))["benchmarks"]:
                if run.get("run_type") != "iteration":
                    continue
                parts = run["run_name"].split("/")
                if parts[0] == "BM_LineRoundTrip":
                    trips[(launch, *map(int, parts[1:4]))].append(run["real_time"] / ROUNDS)
                elif parts[0] in FAMILIES:
                    high = run["producer_cpu_high"]
                    if 0 < high < 1:
                        unplaced += 1
                        continue
                    value = {"ns": run["real_time"] / int(parts[1]), **{c: run[c] for c in COUNTERS}}
                    cells[(parts[0], launch, int(parts[2]), "higher" if high else "lower")].append(value)
    return cells, trips, unplaced


def median_cell(reps):
    return {key: stats.median(r[key] for r in reps) for key in reps[0]}


def ranks(values):
    order = sorted(range(len(values)), key=values.__getitem__)
    result = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            result[order[k]] = (i + j) / 2 + 1
        i = j + 1
    return result


def spearman(x, y):
    return stats.correlation(ranks(x), ranks(y))


def cv(values):
    return stats.stdev(values) / stats.mean(values) * 100


def quartiles(values, digits=1):
    q = stats.quantiles(values, n=4, method="inclusive")
    return " / ".join(f"{v:.{digits}f}" for v in (q[0], stats.median(values), q[2]))


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    lines += ["| " + " | ".join(str(cell) for cell in row) + " |" for row in rows]
    print("\n".join(lines) + "\n")


def caught_up(cell):
    if cell["depth_end"] < 0.01 and cell["pop_waits"] >= 0.01:
        return "caught up"
    if cell["depth_end"] >= 0.10 and cell["pop_waits"] < 0.001:
        return "behind"
    return "between"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("study", type=Path)
    cells, trip_reps, unplaced = load(parser.parse_args().study)
    trips = {key: stats.median(v) for key, v in trip_reps.items()}
    launches = {key[1] for key in cells}
    print(f"{len(launches)} launches; {sum(map(len, cells.values()))} repetitions in {len(cells)} cells; "
          f"{unplaced} repetitions moved the producer between CPUs and belong to no cell\n")
    medians = {key: median_cell(reps) for key, reps in cells.items()}

    print("## Levels\n")
    rows = []
    for family, (label, kind) in FAMILIES.items():
        values = [m["ns"] for key, m in medians.items() if key[0] == family]
        middle = stats.median(values)
        near = sum(middle / 1.25 <= v <= 1.25 * middle for v in values) / len(values)
        slow = f"{sum(v > SLOW_NS for v in values) / len(values):.0%}" if kind == 0 else ""
        rows.append([label, len(values), quartiles(values), f"{min(values):.1f}", f"{max(values):.1f}",
                     f"{near:.0%}", slow, f"{stats.median(cv([r['ns'] for r in rs]) for key, rs in cells.items() if key[0] == family and len(rs) > 1):.1f}%"])
    table(["Benchmark", "Cells", "ns/item Q1 / median / Q3", "Min", "Max", "Cells within 25% of median",
           f"Cells above {SLOW_NS:.0f} ns", "Median CV of a cell's repetitions"], rows)

    print("## The int64 ring's level by region and orientation\n")
    rows = []
    for family in ORIGINAL_INT64:
        mixed = sum(1 for key, rs in cells.items() if key[0] == family
                    and 0 < sum(r["ns"] > SLOW_NS for r in rs) < len(rs))
        regions = defaultdict(dict)
        for (f, launch, index, side), m in medians.items():
            if f == family:
                regions[(launch, index)][side] = m["ns"] > SLOW_NS
        both = [r for r in regions.values() if len(r) == 2]
        pattern = defaultdict(int)
        for r in both:
            pattern[(r["lower"], r["higher"])] += 1
        rows.append([FAMILIES[family][0], len([k for k in cells if k[0] == family]), mixed, len(both),
                     pattern[(False, False)], pattern[(True, False)], pattern[(False, True)], pattern[(True, True)]])
    table(["Benchmark", "Cells", "Cells with slow and fast repetitions", "Regions in both orientations",
           "Fast in both", "Slow with the producer on the lower CPU only", "On the higher CPU only", "Slow in both"],
          rows)
    agree = [(medians[("BM_SPSC_ProducerConsumer", *k[1:])]["ns"] > SLOW_NS) ==
             (medians[k]["ns"] > SLOW_NS)
             for k in medians if k[0] == "BM_SPSC_Local_ProducerConsumer" and ("BM_SPSC_ProducerConsumer", *k[1:]) in medians]
    print(f"Cells measured by both int64 families: {len(agree)}; same level in {sum(agree)}\n")

    print("## Experiment 1: has the consumer caught up?\n")
    rows = []
    for family, (label, kind) in FAMILIES.items():
        groups = defaultdict(list)
        for key, m in medians.items():
            if key[0] == family:
                groups[caught_up(m)].append(m)
        for state in ("behind", "between", "caught up"):
            if groups[state]:
                g = groups[state]
                rows.append([label, state, len(g), f"{stats.median(m['ns'] for m in g):.1f}",
                             *(f"{stats.median(m[c] for m in g):.4f}" for c in COUNTERS[:4])])
    table(["Benchmark", "Consumer", "Cells", "ns/item", "pop_fails", "pop_waits", "depth_end", "push_fails"], rows)
    rows = []
    for family, (label, kind) in FAMILIES.items():
        ms = [m for key, m in medians.items() if key[0] == family]
        row = [label, f"{spearman([m['pop_waits'] for m in ms], [m['ns'] for m in ms]):.2f}"]
        if kind == 0 and family in ORIGINAL_INT64:
            row.append(f"{sum((m['ns'] > SLOW_NS) == (caught_up(m) == 'caught up') for m in ms) / len(ms):.0%}")
        else:
            row.append("")
        rows.append(row)
    table(["Benchmark", "Spearman of ns/item with pop_waits over cells",
           "Cells where slow means caught up"], rows)

    rows = []
    for level in (False, True):
        values = [m["ns"] for key, m in medians.items() if key[0] == "BM_SPSCCached_ProducerConsumer"
                  and ("BM_SPSC_Local_ProducerConsumer", *key[1:]) in medians
                  and (medians[("BM_SPSC_Local_ProducerConsumer", *key[1:])]["ns"] > SLOW_NS) == level]
        rows.append(["slow" if level else "fast", len(values), quartiles(values)])
    table(["Local-checksum ring in the same cell", "Cells", "Cached ring ns/item Q1 / median / Q3"], rows)

    print("## Experiment 2: round trips of the hot lines\n")
    rows = []
    for kind, label, modes in ((0, "int64 ring", (0, 1, 3)), (1, "Order ring", (0, 1, 3)), (3, "mutex state", (0, 1, 2))):
        for mode in modes:
            values = [v for (launch, k, index, m), v in trips.items() if k == kind and m == mode]
            rows.append([label, "lines 1 and 0" if mode == 3 else f"line {mode}", len(values), quartiles(values)])
    table(["Region", "Line", "Regions", "ns per round trip Q1 / median / Q3"], rows)
    print(f"Median CV of one line's round trip over its repetitions: "
          f"{stats.median(cv(v) for v in trip_reps.values()):.1f}%\n")

    rows = []
    for family in ORIGINAL_INT64:
        regions = defaultdict(dict)
        for (f, launch, index, side), m in medians.items():
            if f == family:
                regions[(launch, index)][side] = m["ns"] > SLOW_NS
        groups = defaultdict(list)
        for (launch, index), sides in regions.items():
            if len(sides) == 2:
                gap = trips[(launch, 0, index, 0)] - trips[(launch, 0, index, 1)]
                groups["slow in one orientation" if any(sides.values()) else "fast in both"].append(gap)
        for name, gaps in sorted(groups.items()):
            rows.append([FAMILIES[family][0], name, len(gaps), quartiles(gaps)])
        both = [(trips[(l, 0, i, 0)] - trips[(l, 0, i, 1)], any(s.values())) for (l, i), s in regions.items() if len(s) == 2]
        units = [(trips[(f_l, 0, f_i, 0)] - trips[(f_l, 0, f_i, 1)], m["ns"] > SLOW_NS)
                 for (f, f_l, f_i, side), m in medians.items() if f == family]
        rows.append([FAMILIES[family][0], "regions where slow in one orientation means line 0 is slower", len(both),
                     f"{sum((gap > 0) == slow for gap, slow in both) / len(both):.0%}"])
        rows.append([FAMILIES[family][0], "slow cells whose line 0 is slower", sum(s for _, s in units),
                     f"{sum(1 for gap, s in units if s and gap > 0)}"])
        rows.append([FAMILIES[family][0], "cells whose line 0 is faster that are fast", sum(1 for gap, _ in units if gap <= 0),
                     f"{sum(1 for gap, s in units if not s and gap <= 0)}"])
    table(["Benchmark", "Regions", "Count", "Line 0 minus line 1, ns: Q1 / median / Q3, or result"], rows)

    rows = []
    for family, (label, kind) in FAMILIES.items():
        if family in ORIGINAL_INT64:
            continue
        modes = (0, 1, 2) if kind == 3 else (0, 1)
        ms = [(key, m) for key, m in medians.items() if key[0] == family]
        ns = [m["ns"] for _, m in ms]
        rhos = [f"{spearman([trips[(k[1], kind, k[2], mode)] for k, _ in ms], ns):.2f}" for mode in modes]
        gap = f"{spearman([trips[(k[1], kind, k[2], 0)] - trips[(k[1], kind, k[2], 1)] for k, _ in ms], ns):.2f}"
        within = []
        for side in ("lower", "higher"):
            by_launch = defaultdict(list)
            for k, m in ms:
                if k[3] == side:
                    by_launch[k[1]].append(m["ns"])
            within.append(f"{stats.median(cv(v) for v in by_launch.values() if len(v) >= 4):.1f}%")
        ratio = [medians[(family, k[1], k[2], "higher")]["ns"] / m["ns"] for k, m in ms
                 if k[3] == "lower" and (family, k[1], k[2], "higher") in medians]
        rows.append([label, ", ".join(rhos), gap, " / ".join(within), quartiles(ratio, 2)])
    table(["Benchmark", "Spearman of ns/item with each line's round trip", "With line 0 minus line 1",
           "CV across regions in one launch, producer on lower / higher CPU",
           "Higher over lower CPU, per region: Q1 / median / Q3"], rows)


if __name__ == "__main__":
    main()

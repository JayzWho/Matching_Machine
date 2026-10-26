#!/usr/bin/env python3
"""Tables of the queue-mechanism study.

Usage: python3 scripts/diagnostics/analyze_queue_mechanism.py STUDY [ROOT_RUN]

STUDY is a recording of scripts/diagnostics/queue_mechanism_matrix.json made
with scripts/record/bench_diagnose.py. ROOT_RUN, when given, is a directory
whose raw.tar.gz holds root.json and root.log from one launch of the program
as root.

A cell is one ring of one launch with the producer on one of the launch's two
CPUs (orientation 0 the lower, 1 the higher). Its value is the median over its
repetitions, in nanoseconds of wall-clock time per item, with the counters the
program reports per item. A cell is slow above SLOW_NS per item. One-way times
are medians in TSC ticks. Raw files are read from raw.tar.gz without
extracting it.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import statistics as stats
import tarfile

SLOW_NS = 20.0
INT_FAMILIES = ("BM_S0", "BM_S1", "BM_S2", "BM_S0C", "BM_S1C", "BM_S2C", "BM_S1Fwd", "BM_TraceAsIs",
                "BM_TraceProducerFirst", "BM_TraceConsumerFirst", "BM_Hist", "BM_LagCtl", "BM_Lag0", "BM_Lag4",
                "BM_Lag16", "BM_Lag64", "BM_Padded", "BM_DataSwap", "BM_Alias", "BM_Anon", "BM_TimesAsIs",
                "BM_TimesFar", "BM_S1CFar", "BM_S1FwdFar")
ORDER_FAMILIES = ("BM_OrderS1", "BM_OrderS1C")
LAGS = ("BM_LagCtl", "BM_Lag0", "BM_Lag4", "BM_Lag16", "BM_Lag64")
COUNTERS = ("cycles", "instructions", "stall_sb", "stall_l1d_miss", "recovery", "mo_clears", "xsnp_fwd", "rfo")
TRACE = re.compile(r"MM_TRACE (BM_\w+)/(\d+)/(\d+)/(\d+) call=(\d+) iteration=(\d+) producer_cpu=(\d+) depths=([\d,]+)"
                   r"(?: consumer_ticks=([-\d,]+) producer_ticks=([-\d,]+))?")


def read_runs(data, launch):
    rows = []
    for run in data["benchmarks"]:
        if run.get("run_type") != "iteration":
            continue
        if run.get("time_unit", "ns") != "ns":
            raise SystemExit(f"unexpected time unit in {launch}")
        parts = run["run_name"].split("/")
        rows.append({"launch": launch, "family": parts[0], "args": tuple(int(p) for p in parts[1:4]), **run})
    return rows


def load(study):
    """Benchmark rows and console logs of every complete launch."""
    rows, logs = [], {}
    with tarfile.open(study / "raw.tar.gz", "r:gz") as archive:
        members = {Path(m.name).name: m for m in archive.getmembers() if m.isfile()}
        for name in sorted(members):
            if not re.fullmatch(r"s\d+_b\d+_c\d+\.command\.json", name):
                continue
            launch = name.split(".")[0]
            if not json.load(archive.extractfile(members[name])).get("complete"):
                continue
            rows += read_runs(json.load(archive.extractfile(members[f"{launch}.json"])), launch)
            logs[launch] = archive.extractfile(members[f"{launch}.log"]).read().decode(errors="replace")
    return rows, logs


def median_of(reps):
    keys = set.intersection(*(set(r) for r in reps))
    return {k: stats.median(r[k] for r in reps) for k in keys if isinstance(reps[0][k], (int, float))}


def cells_of(rows):
    """{(family, launch, ring, orientation): median row} for the ring families."""
    groups = defaultdict(list)
    for r in rows:
        if r["family"] in ("BM_OneWay", "BM_LineRoundTrip"):
            continue
        n, ring, orientation = r["args"]
        groups[(r["family"], r["launch"], ring, orientation)].append({**r, "ns": r["real_time"] / n})
    return {key: median_of(reps) for key, reps in groups.items()}


def oneway_of(rows):
    """{(launch, page, line, writer): median one-way ticks and round trip}."""
    groups = defaultdict(list)
    for r in rows:
        if r["family"] == "BM_OneWay":
            groups[(r["launch"], *r["args"])].append(r)
    return {key: median_of(reps) for key, reps in groups.items()}


def roundtrips_of(rows):
    """{(launch, page, mode, CPU of A): median ns per round trip}."""
    groups = defaultdict(list)
    for r in rows:
        if r["family"] == "BM_LineRoundTrip":
            groups[(r["launch"], *r["args"])].append(r["real_time"] / 5000)
    return {key: stats.median(v) for key, v in groups.items()}


def roundtrip_tables(cells, trips):
    print("## Round trips, ns\n")
    pages = defaultdict(dict)
    for key, c in cells.items():
        if key[0] == "BM_S1":
            pages[key[1:3]][key[3]] = slow(c)
    kinds = defaultdict(list)
    for (launch, page), o in pages.items():
        values = [trips.get((launch, page, m, a)) for m, a in ((0, 0), (1, 0), (3, 0), (3, 1))]
        if len(o) == 2 and None not in values:
            kinds[("slow" if o[0] else "fast") + "/" + ("slow" if o[1] else "fast")].append(values)
    table(["Pages, producer on lower/higher CPU", "Pages", "Line 0 (head_)", "Line 1 (tail_)",
           "One-slot ring, lower CPU writes tail_", "One-slot ring, higher CPU writes tail_"],
          [[kind, len(v)] + [quartiles([x[i] for x in v]) for i in range(4)] for kind, v in sorted(kinds.items())])
    # Page level: is the page slow in one orientation?
    flat = [(kind != "fast/fast", v) for kind, v in kinds.items() for v in v]
    right = sum((v[0] > v[1]) == slow_page for slow_page, v in flat)
    print(f"Pages slow in one orientation exactly when line 0 takes longer than line 1: {right}/{len(flat)}.\n")
    # Cell level, with thresholds set on the first half of launches.
    rows = []
    for key, c in cells.items():
        if key[0] != "BM_S1":
            continue
        launch, page, orientation = key[1:]
        v = [trips.get((launch, page, m, a)) for m, a in ((0, 0), (1, 0), (3, orientation), (3, 1 - orientation))]
        if None not in v:
            rows.append({"launch": launch, "orientation": orientation, "slow": slow(c), "line": v[0] - v[1],
                         "ring": v[2], "ring_other": v[3]})
    launches = sorted({r["launch"] for r in rows})
    first = [r for r in rows if r["launch"] in launches[: len(launches) // 2]]
    second = [r for r in rows if r["launch"] in launches[len(launches) // 2:]]
    candidates = {
        "line 0 minus line 1": lambda r: r["line"],
        "one-slot ring with the producer's CPU writing tail_": lambda r: r["ring"],
        "that ring minus the ring in the other orientation": lambda r: r["ring"] - r["ring_other"],
    }
    out = []
    for name, value in candidates.items():
        rule = best_rule(first, value)
        out.append([name, f"{apply_rule(first, value, rule)}/{len(first)}",
                    f"{apply_rule(second, value, rule)}/{len(second)}",
                    f"{'>' if rule[2] > 0 else '<'} {rule[1][0]:.1f} / {rule[1][1]:.1f}"])
    # Both: a page whose line 0 is slower, in the orientation whose ring is the faster one.
    both = lambda r: (r["line"] > 0) and (r["ring"] < r["ring_other"])
    out.append(["line 0 slower and the ring faster than in the other orientation",
                f"{sum(both(r) == r['slow'] for r in first)}/{len(first)}",
                f"{sum(both(r) == r['slow'] for r in second)}/{len(second)}", "no threshold"])
    table(["Rule", "Cells right, first half of launches", "Second half", "Slow when, lower / higher CPU producing"],
          out)


def traces_of(logs):
    """Depth traces, and for the timed families (depths, consumer ticks,
    producer ticks) per iteration."""
    found, timed = defaultdict(list), defaultdict(list)
    for launch, text in logs.items():
        for m in TRACE.finditer(text):
            family, ring, orientation = m.group(1), int(m.group(3)), int(m.group(4))
            depths = [int(v) for v in m.group(8).split(",")]
            found[(family, launch, ring, orientation)].append(depths)
            if m.group(9):
                timed[(family, launch, ring, orientation)].append(
                    (depths, [int(v) for v in m.group(9).split(",")], [int(v) for v in m.group(10).split(",")]))
    return found, timed


def side_speeds(depths, consumer, producer, n=100_000, every=1000, producer_every=1024):
    """Ticks per item of each side by phase. Solo: the producer before the
    consumer's first item. Apart: the queue held at least 2048 items at the
    consumer's checkpoints around the interval. Close: it held fewer than 512.
    Only intervals while the producer was still pushing count."""
    active = [k for k in range(len(depths)) if depths[k] < n - every * k and consumer[k] >= 0]
    apart = [(consumer[k], consumer[k + 1]) for k in active
             if k + 1 in active and depths[k] >= 2048 and depths[k + 1] >= 2048]
    close = [(consumer[k], consumer[k + 1]) for k in active
             if k + 1 in active and depths[k] < 512 and depths[k + 1] < 512]
    result = {"consumer apart": [(b - a) / every for a, b in apart],
              "consumer close": [(b - a) / every for a, b in close]}
    first = consumer[0]
    windows = [(a, b) for a, b in zip(producer, producer[1:]) if a >= 0 and b >= 0]
    result["producer solo"] = [(b - a) / producer_every for a, b in windows if b < first]

    def merged(intervals):
        spans = []
        for a, b in intervals:
            if spans and spans[-1][1] == a:
                spans[-1][1] = b
            else:
                spans.append([a, b])
        return spans

    def inside(intervals):
        spans = merged(intervals)
        return [(b - a) / producer_every for a, b in windows if a >= first and any(s <= a and b <= e for s, e in spans)]
    result["producer apart"] = inside(apart)
    result["producer close"] = inside(close)
    return result


def speeds(cells, timed):
    print("## Each side's speed, TSC ticks per item\n")
    rows = []
    for family in ("BM_TimesAsIs", "BM_TimesFar"):
        for label, want in (("slow", True), ("fast", False)):
            keys = [key for key in timed if key[0] == family and ("BM_S1", *key[1:]) in cells
                    and slow(cells[("BM_S1", *key[1:])]) == want]
            per_cell = defaultdict(list)
            for key in keys:
                merged = defaultdict(list)
                for depths, consumer, producer in timed[key]:
                    for phase, values in side_speeds(depths, consumer, producer).items():
                        merged[phase] += values
                for phase, values in merged.items():
                    if values:
                        per_cell[phase].append(stats.median(values))
            rows.append([family, label, len(keys)] + [
                f"{len(per_cell[p])}: {quartiles(per_cell[p], 1)}" if per_cell[p] else "-"
                for p in ("producer solo", "producer apart", "consumer apart", "producer close", "consumer close")])
    table(["Start", "BM_S1 cells", "Cells", "Producer alone", "Producer, queue >= 2048", "Consumer, queue >= 2048",
           "Producer, queue < 512", "Consumer, queue < 512"], rows)


def slow(cell):
    return cell["ns"] > SLOW_NS


def quartiles(values, digits=1):
    if not values:
        return "-"
    if len(values) < 2:
        return f"{values[0]:.{digits}f}"
    q = stats.quantiles(values, n=4, method="inclusive")
    return " / ".join(f"{v:.{digits}f}" for v in (q[0], stats.median(values), q[2]))


def med(values, digits=1):
    return f"{stats.median(values):.{digits}f}" if values else "-"


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    lines += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    print("\n".join(lines) + "\n")


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
    return stats.correlation(ranks(x), ranks(y)) if len(x) > 2 else float("nan")


def same_state(cells, family, reference="BM_S1"):
    pairs = [(slow(c), slow(cells[(reference, *key[1:])])) for key, c in cells.items()
             if key[0] == family and (reference, *key[1:]) in cells]
    return sum(a == b for a, b in pairs), len(pairs)


def levels(cells):
    print("## Levels and agreement with BM_S1\n")
    rows = []
    for family in INT_FAMILIES + ORDER_FAMILIES:
        values = [c for key, c in cells.items() if key[0] == family]
        if not values:
            continue
        fast_ns = [c["ns"] for c in values if not slow(c)]
        slow_ns = [c["ns"] for c in values if slow(c)]
        agree, total = same_state(cells, family) if family in INT_FAMILIES and family != "BM_Anon" else (0, 0)
        rows.append([family, len(values), f"{len(slow_ns) / len(values):.0%}", quartiles(fast_ns), quartiles(slow_ns),
                     f"{agree}/{total}" if total else ""])
    table(["Benchmark", "Cells", "Slow", "Fast cells, ns/item Q1 / median / Q3", "Slow cells, ns/item",
           "Same state as BM_S1"], rows)


def lags(cells):
    if not any(key[0] == "BM_LagCtl" for key in cells):
        return
    print("## Consumer held behind the producer's line\n")
    keys = [key[1:] for key in cells if key[0] == "BM_LagCtl" and all((f, *key[1:]) in cells for f in LAGS)]
    fast_ref = stats.median(cells[("BM_LagCtl", *k)]["ns"] for k in keys if not slow(cells[("BM_LagCtl", *k)]))
    rows = []
    for label, chosen in (("slow under LagCtl", [k for k in keys if slow(cells[("BM_LagCtl", *k)])]),
                          ("fast under LagCtl", [k for k in keys if not slow(cells[("BM_LagCtl", *k)])])):
        for family in LAGS:
            ns = [cells[(family, *k)]["ns"] for k in chosen]
            cut = [(cells[("BM_LagCtl", *k)]["ns"] - cells[(family, *k)]["ns"]) /
                   (cells[("BM_LagCtl", *k)]["ns"] - fast_ref) for k in chosen] if label.startswith("slow") else []
            waits = [cells[(family, *k)]["pop_waits"] for k in chosen]
            depth = [cells[(family, *k)]["depth_end"] for k in chosen]
            rows.append([label, family, len(chosen), quartiles(ns), quartiles(cut, 2) if cut else "",
                         quartiles(waits, 3), quartiles(depth, 4)])
    table(["Cells", "Benchmark", "Count", "ns/item Q1 / median / Q3", "Share of the gap to the fast level removed",
           "Consumer waits per item", "Depth at the producer's end, share of N"], rows)
    print(f"Fast level for the gap: median of the fast LagCtl cells, {fast_ref:.1f} ns.\n")


def hist(cells):
    if not any(key[0] == "BM_Hist" for key in cells):
        return
    print("## Depth the consumer sees (every 16th item)\n")
    rows = []
    for label, want in (("slow", True), ("fast", False)):
        chosen = [c for key, c in cells.items() if key[0] == "BM_Hist" and slow(c) == want]
        rows.append([label, len(chosen)] + [quartiles([c[k] for c in chosen], 3) for k in
                                            ("same_line", "hist_1_7", "hist_8_63", "hist_64_511", "hist_512")]
                    + [quartiles([c["pop_waits"] for c in chosen], 3)])
    table(["Cells", "Count", "Producer on the consumer's line", "Depth 1-7", "8-63", "64-511", ">= 512",
           "Items the consumer waited for (depth 0)"], rows)


def forwarded(cells):
    print("## Loads another core's cache served, per item, by line\n")
    rows = []
    for family in ("BM_S1Fwd", "BM_S1FwdFar"):
        for label, want in (("slow", True), ("fast", False)):
            chosen = [c for key, c in cells.items() if key[0] == family and ("BM_S1", *key[1:]) in cells
                      and slow(cells[("BM_S1", *key[1:])]) == want]
            if not chosen:
                continue
            for side, name in (("p_fwd_", "producer"), ("c_fwd_", "consumer")):
                data = [sum(c[f"{side}slot{s}"] for s in range(8)) for c in chosen]
                slots = [stats.median(c[f"{side}slot{s}"] for c in chosen) for s in range(8)]
                total = sum(slots) or 1
                rows.append([family, label, name, len(chosen), med([c["ns"] for c in chosen]),
                             med([c[f"{side}head"] for c in chosen], 3), med([c[f"{side}tail"] for c in chosen], 3),
                             med(data, 3), med([c[f"{side}other"] for c in chosen], 3),
                             " ".join(f"{v / total:.2f}" for v in slots)])
    table(["Benchmark", "BM_S1 cell", "Side", "Count", "ns/item", "head_ line", "tail_ line", "Data lines", "Other",
           "Data loads by slot 0-7 of a line, share"], rows)


def counters(cells):
    print("## Hardware counters per item, by side\n")
    rows = []
    for family in ("BM_S0C", "BM_S1C", "BM_S2C", "BM_S1CFar", "BM_OrderS1C"):
        for label, want in (("slow", True), ("fast", False)):
            chosen = [c for key, c in cells.items() if key[0] == family and ("BM_S1", *key[1:]) in cells
                      and slow(cells[("BM_S1", *key[1:])]) == want]
            if not chosen:
                continue
            for side in ("p_", "c_"):
                rows.append([family, label, "producer" if side == "p_" else "consumer", len(chosen),
                             med([c["ns"] for c in chosen])]
                            + [med([c[side + k] for c in chosen], 2 if k in ("mo_clears", "xsnp_fwd", "rfo") else 1)
                               for k in COUNTERS])
    table(["Benchmark", "BM_S1 cell", "Side", "Count", "ns/item", "Cycles", "Instructions", "Store buffer full",
           "L1D-miss load stalls", "Recovery", "Memory-order clears", "Cross-core loads", "RFOs"], rows)
    errors = sorted({(key[0], k) for key, c in cells.items() for k in c if k.endswith("errno")})
    shares = [c[k] for c in cells.values() for k in ("p_running", "c_running") if k in c]
    print(f"Counter errors: {errors or 'none'}; lowest running share {min(shares) if shares else '-'}.\n")


def spin(cells):
    if not any(key[0] == "BM_S0" for key in cells):
        return
    print("## Spin styles\n")
    rows = []
    for family in ("BM_S0", "BM_S1", "BM_S2", "BM_S0C", "BM_S1C", "BM_S2C"):
        chosen = [key for key in cells if key[0] == family]
        slow_cells = [cells[k] for k in chosen if slow(cells[k])]
        rows.append([family, len(chosen), f"{len(slow_cells) / len(chosen):.0%}", quartiles([c["ns"] for c in slow_cells]),
                     quartiles([c["pop_fails"] for c in slow_cells], 2) if "pop_fails" in slow_cells[0] else "",
                     quartiles([c["pop_waits"] for c in slow_cells], 3) if "pop_waits" in slow_cells[0] else ""])
    table(["Benchmark", "Cells", "Slow", "Slow cells, ns/item", "Failed pops per item", "Items waited for"], rows)


def trace_stats(depths, n=100_000, every=1000):
    """Over the checkpoints at which the producer was still pushing (depth
    below N minus the items consumed): the depth before the first item, the
    first checkpoint with depth < 64, the first with depth >= 2048, the
    producer's speed over the consumer's between two checkpoints with depth
    >= 2048, the share of checkpoints after the catch-up with depth < 512, and
    whether the last ten such checkpoints were all below 512 (caught up) or
    all at 2048 or more (producer ahead)."""
    active = [k for k in range(1, len(depths)) if depths[k] < n - every * k]
    caught = next((k for k in active if depths[k] < 64), None)
    escaped = next((k for k in active if depths[k] >= 2048), None)
    ratios = [(every + depths[k + 1] - depths[k]) / every for k in active
              if k + 1 in active and depths[k] >= 2048 and depths[k + 1] >= 2048]
    after = [k for k in active if caught is not None and k >= caught]
    held = sum(depths[k] < 512 for k in after) / len(after) if after else None
    last = [depths[k] for k in active[-10:]]
    end = "caught up" if last and max(last) < 512 else "ahead" if last and min(last) >= 2048 else "mixed"
    return {"start": depths[0], "caught": caught, "escaped": escaped,
            "ratio": stats.median(ratios) if ratios else None, "held": held, "end": end}


def starts(cells, traces):
    if not any(key[0] == "BM_TraceAsIs" for key in cells):
        return
    print("## Starts\n")
    rows = []
    for family in ("BM_TraceAsIs", "BM_TraceProducerFirst", "BM_TraceConsumerFirst"):
        agree, total = same_state(cells, family)
        for label, want in (("slow", True), ("fast", False)):
            keys = [key for key in cells if key[0] == family and slow(cells[("BM_S1", *key[1:])]) == want
                    and ("BM_S1", *key[1:]) in cells]
            found = [trace_stats(t) for key in keys for t in traces.get(key, [])]
            caught = [s["caught"] * 1000 for s in found if s["caught"] is not None]
            escaped = [s["escaped"] * 1000 for s in found if s["escaped"] is not None]
            ends = defaultdict(int)
            for s in found:
                ends[s["end"]] += 1
            rows.append([family, label, f"{agree}/{total}", len(keys), len(found),
                         quartiles([s["start"] for s in found], 0),
                         f"{len(caught)}: " + quartiles(caught, 0), f"{len(escaped)}: " + quartiles(escaped, 0),
                         quartiles([s["ratio"] for s in found if s["ratio"] is not None], 2),
                         quartiles([s["held"] for s in found if s["held"] is not None], 2),
                         " ".join(f"{k} {v}" for k, v in sorted(ends.items())),
                         quartiles([cells[k]["ns"] for k in keys])])
    table(["Start", "BM_S1 cells", "Same state as BM_S1", "Cells", "Iterations", "Depth before the first item",
           "Iterations caught up: items until depth < 64", "Iterations ahead: items until depth >= 2048",
           "Producer over consumer speed at depth >= 2048", "Share of checkpoints with depth < 512 after catching up",
           "State over the last ten checkpoints", "ns/item"], rows)


def control_page(cells):
    if not any(key[0] == "BM_DataSwap" for key in cells):
        return
    print("## What the memory changes\n")
    rows = []
    for family in ("BM_DataSwap", "BM_Alias"):
        agree, total = same_state(cells, family)
        rows.append([family, f"{agree}/{total}"])
    table(["Benchmark", "Cells in the same state as BM_S1 on the same control page"], rows)
    composite = [c for key, c in cells.items() if key[0] == "BM_S1"]
    anon = [c for key, c in cells.items() if key[0] == "BM_Anon"]
    print(f"Slow cells: BM_S1 {sum(map(slow, composite))}/{len(composite)}, "
          f"BM_Anon {sum(map(slow, anon))}/{len(anon)}.\n")
    # A control page in both orientations.
    pages = defaultdict(dict)
    for key, c in cells.items():
        if key[0] == "BM_S1":
            pages[key[1:3]][key[3]] = slow(c)
    kinds = defaultdict(int)
    for both in pages.values():
        if len(both) == 2:
            kinds[(both[0], both[1])] += 1
    table(["Producer on the lower CPU", "Producer on the higher CPU", "Control pages"],
          [["slow" if a else "fast", "slow" if b else "fast", kinds[(a, b)]]
           for a, b in ((False, False), (True, False), (False, True), (True, True))])


def handoffs(cells, oneway):
    """Per control page and orientation: the one-way time of head_ from the
    consumer to the producer and of tail_ from the producer to the consumer."""
    rows = []
    for key, c in cells.items():
        if key[0] != "BM_S1":
            continue
        launch, page, orientation = key[1:]
        # Writer 0 is the lower CPU. The consumer writes head_; the producer writes tail_.
        head = oneway.get((launch, page, 0, 1 - orientation))
        tail = oneway.get((launch, page, 1, orientation))
        head_back = oneway.get((launch, page, 0, orientation))
        tail_back = oneway.get((launch, page, 1, 1 - orientation))
        if head and tail and head_back and tail_back:
            rows.append({"launch": launch, "page": page, "orientation": orientation, "slow": slow(c), "ns": c["ns"],
                         "head": head["ow"], "tail": tail["ow"], "head_back": head_back["ow"],
                         "tail_back": tail_back["ow"]})
    return rows


def best_rule(rows, value):
    """The threshold on value(row) that best separates slow from fast cells,
    per orientation: (accuracy, threshold per orientation, direction)."""
    best = None
    for direction in (1, -1):
        thresholds, correct = {}, 0
        for orientation in (0, 1):
            group = [r for r in rows if r["orientation"] == orientation]
            options = sorted({value(r) for r in group})
            scored = max(((sum((direction * value(r) > direction * t) == r["slow"] for r in group), t)
                          for t in options), default=(0, 0))
            thresholds[orientation] = scored[1]
            correct += scored[0]
        if best is None or correct > best[0]:
            best = (correct, thresholds, direction)
    return best


def apply_rule(rows, value, rule):
    _, thresholds, direction = rule
    return sum((direction * value(r) > direction * thresholds[r["orientation"]]) == r["slow"] for r in rows)


def oneway_tables(cells, oneway):
    print("## One-way handoffs of the index lines, TSC ticks\n")
    rows = handoffs(cells, oneway)
    if not rows:
        return
    by_page = defaultdict(dict)
    for r in rows:
        by_page[(r["launch"], r["page"])][r["orientation"]] = r
    kinds = defaultdict(list)
    for both in by_page.values():
        if len(both) == 2:
            kinds[("slow" if both[0]["slow"] else "fast") + "/" + ("slow" if both[1]["slow"] else "fast")].append(both)
    # Per page: head_ and tail_ in each direction, by writer CPU.
    out = []
    for kind, pages in sorted(kinds.items()):
        def line(page_rows, field, orientation):
            return [p[orientation][field] for p in page_rows]
        out.append([kind, len(pages),
                    med(line(pages, "head", 0), 0), med(line(pages, "head", 1), 0),
                    med(line(pages, "tail", 1), 0), med(line(pages, "tail", 0), 0)])
    table(["Pages, producer on lower/higher CPU", "Pages", "head_ from higher to lower CPU",
           "head_ from lower to higher CPU", "tail_ from higher to lower CPU", "tail_ from lower to higher CPU"], out)
    launches = sorted({r["launch"] for r in rows})
    first = [r for r in rows if r["launch"] in launches[: len(launches) // 2]]
    second = [r for r in rows if r["launch"] in launches[len(launches) // 2:]]
    candidates = {
        "head_ handoff to the producer minus tail_ handoff to the consumer": lambda r: r["head"] - r["tail"],
        "head_ handoff to the producer": lambda r: r["head"],
        "tail_ handoff to the consumer": lambda r: r["tail"],
        "head_ minus tail_, both directions summed": lambda r: r["head"] + r["head_back"] - r["tail"] - r["tail_back"],
    }
    out = []
    for name, value in candidates.items():
        rule = best_rule(first, value)
        out.append([name, f"{apply_rule(first, value, rule)}/{len(first)}", f"{apply_rule(second, value, rule)}/{len(second)}",
                    f"{'>' if rule[2] > 0 else '<'} {rule[1][0]:.0f} / {rule[1][1]:.0f}"])
    table(["Rule: slow when the value passes a threshold set per orientation", "Cells right, first half of launches",
           "Second half, same thresholds", "Slow when, lower / higher CPU producing"], out)


def order(cells):
    if not any(key[0] == "BM_OrderS1" for key in cells):
        return
    print("## Order ring against the int64 ring on the same control page\n")
    rows = []
    for family in ORDER_FAMILIES:
        for label, want in (("int64 slow", True), ("int64 fast", False)):
            chosen = [c for key, c in cells.items() if key[0] == family and ("BM_S1", *key[1:]) in cells
                      and slow(cells[("BM_S1", *key[1:])]) == want]
            rows.append([family, label, len(chosen), quartiles([c["ns"] for c in chosen]),
                         quartiles([c["pop_waits"] for c in chosen], 3)])
    table(["Benchmark", "Same control page and orientation in BM_S1", "Cells", "ns/item", "Consumer waits per item"], rows)


def xor_fit(samples, low=6, high=38):
    """For each bit of the slice number, the physical-address bits whose XOR,
    with a constant, reproduces it over all samples (address, slice), by
    Gaussian elimination over GF(2). Returns [(bits, constant, samples
    reproduced, rank)] per slice bit."""
    width = high - low
    fits = []
    for out in (0, 1):
        rows = [[(a >> (low + i)) & 1 for i in range(width)] + [1, (s >> out) & 1] for a, s in samples]
        pivots, r = [], 0
        for col in range(width + 1):
            pivot = next((i for i in range(r, len(rows)) if rows[i][col]), None)
            if pivot is None:
                continue
            rows[r], rows[pivot] = rows[pivot], rows[r]
            for i in range(len(rows)):
                if i != r and rows[i][col]:
                    rows[i] = [x ^ y for x, y in zip(rows[i], rows[r])]
            pivots.append(col)
            r += 1
        solution = [0] * (width + 1)
        for i, col in enumerate(pivots):
            solution[col] = rows[i][-1]
        reproduced = sum((sum(solution[i] * ((a >> (low + i)) & 1) for i in range(width)) + solution[width]) % 2
                         == (s >> out) & 1 for a, s in samples)
        fits.append(([low + i for i in range(width) if solution[i]], solution[width], reproduced, len(pivots)))
    return fits


def root(text, cells_root, oneway_root, trips_root):
    print("## Run as root: LLC slices of the index lines\n")
    scores = re.findall(r"MM_SLICE_SCORE config=(0x[0-9a-f]+) clean=(\d+)/64", text)
    print("Event encodings, lines of control page 0 that fell to one slice: "
          + ", ".join(f"{c} {n}/64" for c, n in scores) + "\n")
    # The slice serving a line is the box whose lookups exceed every other
    # box's by at least half of the 10,000 loads; lookups from other cores add
    # a similar count to every box.
    def box_of(counts, loops=10_000):
        order = sorted(range(4), key=counts.__getitem__)
        return order[-1] if counts[order[-1]] - counts[order[-2]] >= loops // 2 else -1
    slices, samples = {}, []
    for m in re.finditer(r"MM_SLICE config=\S+ (page|anon)=(\d+) line=(\d) pfn=(0x[0-9a-f]+) box=-?\d+ "
                         r"counts=([\d,]+)", text):
        kind, index, line, pfn = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4), 16)
        box = box_of([int(v) for v in m.group(5).split(",")])
        if kind == "page":
            slices[(index, line)] = box
        if box >= 0 and pfn:
            samples.append(((pfn << 12) | (64 * line), box))
    best = max(scores, key=lambda s: int(s[1]))[0] if scores else None
    page0 = re.search(r"MM_PFN pool_page=0 pfn=(0x[0-9a-f]+)", text)
    for m in re.finditer(rf"MM_SLICE_CAL config={best} line=(\d+) counts=([\d,]+)", text):
        box = box_of([int(v) for v in m.group(2).split(",")])
        if page0 and box >= 0:
            samples.append(((int(page0.group(1), 16) << 12) | (64 * int(m.group(1))), box))
    unresolved = sum(1 for v in slices.values() if v < 0)
    print(f"Control-page lines with one slice ahead of the others by at least 5,000 lookups: "
          f"{len(slices) - unresolved}/{len(slices)}.\n")
    pairs = defaultdict(int)
    for page in range(64):
        if (page, 0) in slices and (page, 1) in slices:
            pairs[(slices[(page, 0)], slices[(page, 1)])] += 1
    print("Slice of head_ and tail_ per control page: "
          + ", ".join(f"({h}, {t}) {n}" for (h, t), n in sorted(pairs.items())) + "\n")
    for out, (bits, constant, reproduced, rank) in enumerate(xor_fit(samples)):
        print(f"Slice bit {out} = XOR of address bits {bits}{' and 1' if constant else ''}: "
              f"{reproduced}/{len(samples)} samples, rank {rank}.")
    print()
    combos = defaultdict(lambda: [0, 0])
    page_kind = defaultdict(dict)
    for key, c in cells_root.items():
        if key[0] != "BM_S1" or (key[2], 0) not in slices or (key[2], 1) not in slices:
            continue
        combos[(slices[(key[2], 0)], slices[(key[2], 1)], key[3])][slow(c)] += 1
        page_kind[key[2]][key[3]] = slow(c)
    table(["head_ slice", "tail_ slice", "Producer CPU", "Fast cells", "Slow cells"],
          [[h, t, "lower" if o == 0 else "higher", f, s] for (h, t, o), (f, s) in sorted(combos.items())])
    predicted = sum(max(v) for v in combos.values())
    total = sum(sum(v) for v in combos.values())
    print(f"The majority state of each slice pair and orientation matches {predicted}/{total} cells.\n")
    # Handoffs and round trips by slice pair.
    out = []
    for pair in sorted(pairs):
        pages = [p for p in range(64) if (slices.get((p, 0)), slices.get((p, 1))) == pair]
        def values(line, writer):
            return [oneway_root[("root", p, line, writer)]["ow"] for p in pages if ("root", p, line, writer) in oneway_root]
        def trip(mode, a):
            return [trips_root[("root", p, mode, a)] for p in pages if ("root", p, mode, a) in trips_root]
        out.append([f"({pair[0]}, {pair[1]})", len(pages), med(values(0, 0), 0), med(values(0, 1), 0),
                    med(values(1, 0), 0), med(values(1, 1), 0), med(trip(0, 0)), med(trip(1, 0)),
                    med(trip(3, 0)), med(trip(3, 1))])
    table(["Slices of head_, tail_", "Pages", "head_ lower to higher, ticks", "head_ higher to lower",
           "tail_ lower to higher", "tail_ higher to lower", "Round trip line 0, ns", "Line 1",
           "One-slot ring, lower CPU writes tail_", "Higher CPU writes tail_"], out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("study", type=Path)
    parser.add_argument("root_run", type=Path, nargs="?")
    args = parser.parse_args()
    rows, logs = load(args.study)
    cells, oneway = cells_of(rows), oneway_of(rows)
    traces, timed = traces_of(logs)
    misplaced = sum(c.get("misplaced", 0) > 0 for c in cells.values())
    print(f"{len(logs)} launches; {len(cells)} cells; {misplaced} cells with a thread on the wrong CPU\n")
    levels(cells)
    lags(cells)
    hist(cells)
    forwarded(cells)
    counters(cells)
    spin(cells)
    starts(cells, traces)
    if timed:
        speeds(cells, timed)
    control_page(cells)
    oneway_tables(cells, oneway)
    trips = roundtrips_of(rows)
    if trips:
        roundtrip_tables(cells, trips)
    order(cells)
    if args.root_run:
        with tarfile.open(args.root_run / "raw.tar.gz", "r:gz") as archive:
            root_rows = read_runs(json.load(archive.extractfile("root.json")), "root")
            root_log = archive.extractfile("root.log").read().decode(errors="replace")
        root_cells = cells_of(root_rows)
        root(root_log, root_cells, oneway_of(root_rows), roundtrips_of(root_rows))
        _, root_timed = traces_of({"root": root_log})
        if root_timed:
            speeds(root_cells, root_timed)


if __name__ == "__main__":
    main()

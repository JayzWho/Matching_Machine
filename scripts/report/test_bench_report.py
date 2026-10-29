#!/usr/bin/env python3
"""Check the launch test of bench_report.py on synthetic launches.

Usage:
    scripts/report/test_bench_report.py

Draws 16 launches of 5 repetitions with 2% scatter, 100 times for each of three
cases, and counts how often the test marks the benchmark as moving between
launches. The test must stay quiet when the launches do not differ and must
find both a gradual difference and a benchmark that settles on one of two
levels at each launch. Also checks that rows marked as a warm-up are left out,
and that rows of studies recorded before the mark existed all count, and that
the latency table states the calibrated TSC frequency when a study has one,
keeps queue time and service time apart and reads the reports of studies
recorded before the split as end-to-end latency, that the pipeline
throughput benchmarks are found under both of their names, and that the
producer-to-consumer instances are recognised, split into a fast and a slow
tier despite an outlier, and summarized by their median and quartiles.
"""
import random
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_report  # noqa: E402

TRIALS, PERMUTATIONS, LAUNCHES, REPETITIONS, SCATTER = 100, 1000, 16, 5, 0.02
CASES = (
    # name, factor applied to a whole launch, threshold on p, allowed number of marks
    ("launches do not differ", lambda g: 1.0, 0.05, range(0, 13)),
    ("each launch shifted by a random 2%", lambda g: 1.0 + g.gauss(0.0, 0.02), bench_report.ALPHA, range(80, 101)),
    ("each launch on one of two levels 38% apart", lambda g: 1.38 if g.random() < 0.4 else 1.0,
     bench_report.ALPHA, range(90, 101)),
)


def warmup_rows_left_out():
    marked = [{"repetition": "0", "warmup": "1"}, {"repetition": "1", "warmup": "0"}]
    unmarked = [{"repetition": "0"}, {"repetition": "1"}]
    return (bench_report.counted(marked) == marked[1:] and bench_report.counted(unmarked) == unmarked)


def tsc_frequency_stated():
    def env(*ghz):
        return {"sessions": [{"before": {"tsc": {"calibration": {"ghz": g}}}} for g in ghz]}
    stated = bench_report.unit_note(env(2.419182, 2.419183))
    return ("2.419182 to 2.419183 GHz (2 calibrations" in stated
            and "has not been calibrated" in bench_report.unit_note({"sessions": [{"before": {}}]}))


def latency_quantities_kept_apart():
    def row(quantity, repetition, ticks):
        found = {"condition": "cleared", "binary": "bench_matching_engine", "session": "1", "round": "1",
                 "report": str(repetition), "warmup": str(int(repetition == 0)), "percentile": "P50",
                 "tsc_ticks": str(ticks)}
        if quantity:
            found["quantity"] = quantity
        return found
    split = bench_report.latency_groups([row("queue", 0, 5), row("service", 0, 7), row("queue", 1, 900),
                                         row("service", 1, 600)])
    before = bench_report.latency_groups([row(None, 0, 5), row(None, 1, 950)])
    return (split == {("cleared", "SPSC pipeline", "queue", "P50"): {(1, 1): [900]},
                      ("cleared", "SPSC pipeline", "service", "P50"): {(1, 1): [600]}}
            and before == {("cleared", "SPSC pipeline", "end-to-end", "P50"): {(1, 1): [950]}})


def sampled_names_parsed():
    name = "BM_SPSC_ProducerConsumer/100000/block:3/placement:1/iterations:10/real_time"
    return (bench_report.sampled_instance(name) == ("BM_SPSC_ProducerConsumer", 100000, 3, 1)
            and bench_report.sampled_instance("BM_SPSC_ProducerConsumer/100000/real_time") is None)


def tiers_split():
    """16 launches of 16 regions in two placements: a region is slow in placement
    0 or in placement 1 with a chance of a quarter each, never in both; one slow
    instance is disturbed eightfold."""
    gen, launches = random.Random(7), {}
    for launch in range(16):
        cells = {}
        for region in range(16):
            slow_in = gen.choice((0, 1, None, None))
            for placement in (0, 1):
                cells[(region, placement)] = (60.0 if slow_in == placement else 9.0) * (1 + gen.gauss(0, 0.02))
        launches[launch] = cells
    disturbed = next(cell for cell, v in launches[0].items() if v > 30)
    launches[0][disturbed] = 480.0
    s = bench_report.tier_summary(launches)
    return (20 < s["threshold"] < 30 and s["gap"] > 3 and s["both"] == 0
            and all(0.15 < share < 0.35 for share in s["share"].values())
            and abs(bench_report.median(s["fast"]) - 9.0) < 0.2
            and abs(bench_report.median(s["slow"]) - 60.0) < 1.5)


def spread_summarized():
    meds, q1s, q3s = bench_report.spread_summary({1: {(r, p): float(2 * r + p + 1) for r in range(16) for p in (0, 1)}})
    return meds == [16.5] and q1s == [8.75] and q3s == [24.25]


def pipeline_names_matched():
    old, new = "BM_Pipeline_Throughput/100000/iterations:3", "BM_Pipeline_Throughput/100000/iterations:3/real_time"
    queue = "BM_SPSC_ProducerConsumer/100000/real_time"
    return (bench_report.base_name(old) == old and bench_report.base_name(new) == old
            and bench_report.base_name(queue) == queue)


def main():
    gen, failed = random.Random(1), False
    for name, check in (("warm-up rows left out, unmarked rows counted", warmup_rows_left_out),
                        ("calibrated TSC frequency stated", tsc_frequency_stated),
                        ("latency quantities kept apart", latency_quantities_kept_apart),
                        ("producer-to-consumer instances recognised", sampled_names_parsed),
                        ("int64 tiers split despite an outlier", tiers_split),
                        ("median and quartiles over the instances", spread_summarized),
                        ("pipeline throughput found under both names", pipeline_names_matched)):
        ok = check()
        failed |= not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
    for name, factor, threshold, allowed in CASES:
        marks = 0
        for trial in range(TRIALS):
            groups = []
            for _ in range(LAUNCHES):
                level = 100.0 * factor(gen)
                groups.append([level * (1.0 + gen.gauss(0.0, SCATTER)) for _ in range(REPETITIONS)])
            _, _, p = bench_report.launch_test(groups, PERMUTATIONS, random.Random(trial))
            marks += p < threshold
        ok = marks in allowed
        failed |= not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}: p < {threshold} in {marks} of {TRIALS} "
              f"(allowed {allowed.start} to {allowed.stop - 1})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

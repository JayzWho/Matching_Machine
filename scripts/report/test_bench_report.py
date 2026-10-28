#!/usr/bin/env python3
"""Check the launch test of bench_report.py on synthetic launches.

Usage:
    scripts/report/test_bench_report.py

Draws 16 launches of 5 repetitions with 2% scatter, 100 times for each of three
cases, and counts how often the test marks the benchmark as moving between
launches. The test must stay quiet when the launches do not differ and must
find both a gradual difference and a benchmark that settles on one of two
levels at each launch. Also checks that rows marked as a warm-up are left out,
and that rows of studies recorded before the mark existed all count.
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


def main():
    gen, failed = random.Random(1), False
    ok = warmup_rows_left_out()
    failed |= not ok
    print(f"{'ok  ' if ok else 'FAIL'} warm-up rows left out, unmarked rows counted")
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

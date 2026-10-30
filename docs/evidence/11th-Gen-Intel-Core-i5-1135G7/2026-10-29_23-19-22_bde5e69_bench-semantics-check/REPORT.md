# Single-threaded benchmark fixes: rough check of 2026-10-29

Branch `bench/order-book-semantics` fixes what the single-threaded order book
benchmarks time and runs them on the v1 order book as well. This check has two
parts: a rough check that the benchmarks the branch does not mean to change
stay as they were, and one sanity run of the changed benchmarks. Neither is a
measurement; the new control is recorded later, in measurement mode.

## Design

This is not a recording. It ran outside measurement mode (`powersave`
governor, turbo on, SMT siblings online, device interrupts not steered) on an
otherwise idle machine, from 23:19 to 23:28.

- **Rough check.** `scripts/diagnostics/bench_semantics_check.sh` runs master
  (`4227406`) and the branch six times each, alternating, one repetition per
  run: `bench_order_book` filtered to `BM_AddOrder_NoMatch`
  (`BM_AddOrder_NoMatch_V3` on the branch) on CPU 2, and
  `bench_matching_engine` filtered to `SingleThread|Pipeline_Throughput` on
  CPUs 2 and 3.
- **Sanity run.** Once each, three repetitions: master's and the branch's
  `bench_order_book` on CPU 2, and the branch's single-threaded pair in
  `bench_matching_engine` on CPUs 2 and 3.

The branch's binaries are byte-identical to a build of the branch's head,
`bde5e69`; its last two commits add only ctest entries and a check in the
recorders. `bench_baseline_mutex` is byte-identical between master and the
branch and was not run.

i5-1135G7, kernel 7.0.0-34-generic, GCC 15.2.0 with `-O2 -DNDEBUG -std=c++20`.
`env.json` was captured after the runs; it marks the tree dirty only because
of two untracked `__pycache__` directories. `raw.tar.gz` holds one JSON file
per run; `scripts/diagnostics/bench_semantics_check.py raw.tar.gz` prints both
tables below.

## Rough check

Median of six runs, in ns per iteration.

| Benchmark | master (ns) | branch (ns) | change | CV master | CV branch |
|---|---:|---:|---:|---:|---:|
| `BM_AddOrder_NoMatch` | 22 | 22 | -0.1% | 2.1% | 1.5% |
| `BM_SingleThread_Baseline` | 718,936 | 726,685 | +1.1% | 1.3% | 0.6% |
| `BM_SingleThread_Baseline_AddOrder` | | 791,398 | | | 0.3% |
| `BM_Pipeline_Throughput/10000/iterations:3/real_time` | 2,544,213 | 2,067,005 | -18.8% | 10.7% | 15.1% |
| `BM_Pipeline_Throughput/50000/iterations:3/real_time` | 7,510,725 | 7,524,219 | +0.2% | 1.6% | 0.7% |
| `BM_Pipeline_Throughput/100000/iterations:3/real_time` | 15,402,027 | 15,532,946 | +0.9% | 1.3% | 1.7% |

## Sanity run

Median of three repetitions, in ns per iteration. The last column is the
branch's counter of the work done per iteration, identical for v3 and v1.

| Benchmark | master (ns) | branch, v3 (ns) | branch, v1 (ns) | per iteration |
|---|---:|---:|---:|---|
| `BM_AddOrder_NoMatch` | 22.5 | 21.9 | 28.9 |  |
| `BM_AddOrder_FullMatch` | 230 | 82.2 | 102 | trades: 1 |
| `BM_AddOrder_SweepLevels/1` | 266 | 80.4 | 102 | trades: 1 |
| `BM_AddOrder_SweepLevels/5` | 386 | 278 | 432 | trades: 5 |
| `BM_AddOrder_SweepLevels/10` | 523 | 512 | 803 | trades: 10 |
| `BM_AddOrder_SweepLevels/20` | 919 | 1,054 | 1,604 | trades: 20 |
| `BM_CancelOrder` | 25,628 | 118 | 160 | cancels: 1 |
| `BM_MixedWorkload` | 79,253,306 | 127,107,373 | 191,092,187 | trades: 697,471 |
| `BM_SingleThread_Baseline` |  | 713,042 |  | trades: 6,250 |
| `BM_SingleThread_Baseline_AddOrder` |  | 784,008 |  | trades: 6,250 |

## Conclusion

- The benchmarks the branch does not mean to change move by at most 1.1%.
  `BM_AddOrder_NoMatch_V3` compiles to the same instructions as master's
  `BM_AddOrder_NoMatch` apart from the timestamp constant. The pipeline at
  10,000 orders is too short to read outside measurement mode (CV 11–15%).
- The changed benchmarks pass their own checks, and the numbers are plausible:
  a cancel takes about 120 ns instead of 26 µs of mostly destruction; a sweep
  grows with the number of levels; `BM_MixedWorkload` takes about 60% longer
  than master's because every iteration now does the full work.
- A sweep costs about 51 ns per level on the branch against about 34 ns in
  master, where each iteration's levels were rested just before it was timed.
  A likely reason, not checked: a batch rests about 1,000 levels, more than
  the L1 data cache holds, so the levels a sweep takes have mostly left it.
- On the same stream, `add_order_noalloc` takes about 9% less time than
  `add_order`, about 7 ns per order.
- With six repetitions per launch, as the recorder runs it, a launch of
  `bench_order_book` takes about 2.4 minutes instead of about 1.7.

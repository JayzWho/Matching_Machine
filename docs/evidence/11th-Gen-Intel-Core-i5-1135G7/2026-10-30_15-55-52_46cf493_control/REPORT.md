# Control of the fixed benchmarks

Recorded on 2026-10-30 on the 11th Gen Intel Core i5-1135G7 at commit `46cf493`.

This is the current control: the four benchmark programs after the fixes
listed in [`docs/AUDIT.md`](../../../AUDIT.md), on the code they measure at
that commit. [`docs/RESULTS.md`](../../../RESULTS.md) reports it. It replaces the control of
2026-10-15, which measured the unfixed benchmarks and is kept at the tag
`archive/control-2026-10-15`.

## How it was recorded

`scripts/record/bench_record.py --label control`: 4 sessions of 8 rounds, 32
launches of each program, half in `shared` and half in `cleared`, 6
repetitions per benchmark and launch with the first a warm-up. A launch is
represented by the median of its five counted repetitions. Measurement mode
held the cores at the 2.4 GHz base frequency with turbo off and the SMT
siblings offline; the programs ran on CPUs 3 and 1. GCC 15.2.0 with
`-O2 -DNDEBUG -std=c++20`, kernel 7.0.0-34-generic. The TSC calibrated at
2.419182 to 2.419183 GHz in all 8 calibrations. The VirtualBox VM was saved
for the whole study, which took 1 hour 44 minutes.

## Validity

All 128 launches had 0 thermal throttle events, 0 s suspended, and 0 device
and 0 thermal interrupts on the measurement cores. Launches on one core read
2400 MHz throughout, and the package stayed at 42 to 55 °C. Other tasks used a
median of 4.4 ms of CPU time per second on the launch's cores in `shared`
(at most 21.9) and 1.2 ms in `cleared` (at most 5.6). For every benchmark the
two conditions give medians within 2% of each other, so the table below and
`docs/RESULTS.md` pool all 32 launches. The mutex pipeline's queue time is the
exception, as finding 1 explains.

## Findings

**1. The results are steady from launch to launch.** CV is the coefficient of
variation of the 32 launch medians.

| Benchmarks | CV of launch medians |
|---|---:|
| Order book, v3 and v1, except `BM_AddOrder_NoMatch_V3` (finding 2) | 0.2% to 1.7% |
| `BM_SingleThread_Baseline`, `BM_SingleThread_Baseline_AddOrder` | 0.2%, 0.3% |
| SPSC pipeline throughput, 10,000 to 100,000 orders | 2.1% to 3.4% |
| Mutex pipeline throughput, 10,000 to 100,000 orders | 2.7% to 4.9% |
| Service time P50, SPSC and mutex pipelines | 1.2%, 0.2% |
| int64 ring, level of each tier at 100,000 items (fast, slow) | 2.3%, 2.9% |
| Order ring, median over a launch's 32 instances | 2.3% |

The mutex pipeline's queue time is the exception by design: its queue has no
bound, and its P50 moves with a CV of 26% between launches.

**2. `BM_AddOrder_NoMatch_V3` is sometimes slow for a whole repetition.** Its
usual level is 37.8 ns, but 32 of its 160 counted repetitions took 44 to 93 ns.
They occur in 17 of the 32 launches, mixed with normal repetitions of the same
launch. The v1 order book in the same benchmark code stays within 49.8 to
52.0 ns in every repetition. The slow repetitions come with page faults: a
launch without them faulted 450,000 to 612,000 times, one with them up to
1,911,000 times, and the rank correlation between a launch's total time in this
benchmark and its faults is 0.94. Twenty launches outside the recorder, in
and out of measurement mode, alone and with every benchmark, gave 120
repetitions without a slow one and a constant fault count. The cause is not established. Because
only 3 launch medians are affected, the median of launch medians, 38.2 ns, is
about 1% above the usual level; a single launch's ratio of v1 to v3 ranges from
0.61 to 1.38.

**3. Two rarer events.** In one launch (session 4, round 2) every instance of
`BM_Mutex_ProducerConsumer` ran about 35% slower, while the other benchmarks of
that launch and its telemetry were normal; without it, the mutex queue's CV is
1.7% at 10,000 items and 1.8% at 100,000. In 3 of 32 launches `BM_SPSC_SingleThread_Push` ran at 1.3 to 2.1 ns per
push instead of 1.1 ns throughout the launch.

**4. Checks carried over from the two-thread fixes.** No SPSC latency report
started its two workers on one CPU: the highest maximum queue time is 1.04
times the median maximum. The median queue time divided by the 4,095 orders
ahead of each order gives 355 ns per order in the latency-report run, against
286 ns in the throughput runs: the fences slow the consumer by about 24%. The
first launch of the study gives the Order ring 8% below the median of
launches.

## Reproduce

```bash
cmake --build build/release -j"$(nproc)"
scripts/record/bench_record.py --label control
scripts/report/bench_report.py docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control
```

The last command prints the full tables by condition: per-benchmark launch
statistics, the producer-to-consumer tiers and medians, round-by-round medians,
`shared` against `cleared`, thread scheduling and the latency percentiles.

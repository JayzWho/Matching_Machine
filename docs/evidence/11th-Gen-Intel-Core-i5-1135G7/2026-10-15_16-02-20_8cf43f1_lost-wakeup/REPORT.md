# Mutex pipeline before and after the lost wake-up fix

Recorded on 2026-10-15 on the 11th Gen Intel Core i5-1135G7 at commit `8cf43f1`.

The producer of the mutex baseline used to set its completion flag and notify
the consumer without holding the queue mutex, so the consumer could miss the
notification and wait forever. Commit `9bd30eb` sets the flag under the mutex.
This study asks two questions: does the unfixed code hang on demand and the
fixed code not, and does the fix change the throughput of the mutex pipeline?
The fix adds one lock and unlock at the end of each pipeline run, so no change
in throughput is expected.

An **offset** is the engine object's address modulo 64. The
[launch-variance investigation](../2026-10-14_13-32-00_65a2223_launch-variance/REPORT.md)
found that offsets 0 and 48 give the slower of the pipeline's two levels and
offsets 16 and 32 the faster one.

## Hang reproduction

`scripts/diagnostics/mutex_shutdown_stress.cpp` includes one version of the
benchmark source, runs its pipeline back to back with a given number of orders,
and reports a hang when no run completes for two seconds. It ran outside
measurement mode, pinned with `taskset -c 1,3`. The table was recorded with a
draft of the program that differed only in its argument checks; the committed
version, built against the unfixed source, hung in 3 of 3 further runs at 10
orders.

| Orders | Unfixed source | Fixed source |
|---:|---|---|
| 1 | No hang in 120 s, 4,172,704 runs | No hang in 120 s, 4,095,828 runs |
| 10 | Hung in 6 of 6 runs, after 36,764 to 1,363,490 completed pipelines | No hang in 300 s, 9,613,835 runs |
| 100 | No hang in 120 s, 2,052,657 runs | No hang in 120 s, 2,007,292 runs |
| 1,000 | Hung in 5 of 6 runs, after 1,579 to 21,149 completed pipelines; the sixth completed 251,406 in 120 s | No hang in 300 s, 936,514 runs |
| 10,000 | No hang in 120 s, 32,982 runs | No hang in 120 s, 31,876 runs |

## Throughput

`scripts/bench_diagnose.py` recorded `scripts/diagnostics/lost_wakeup_matrix.json`:
two sessions of eight blocks, each block launching the four conditions once in
rotating order, five repetitions per launch, so 16 launches per condition. The
conditions are placement builds made by `scripts/diagnostics/build_mutex_layout.py`
from the unfixed source (`benchmarks/bench_baseline_mutex.cpp` as of `4408236`)
and from the fixed source (`9bd30eb`), each with the engine at offset 0 and at
offset 16. Measurement mode was active: 2400 MHz, turbo off, SMT siblings
offline, measurement cores 3 and 1. Every task of the user was kept on cores 0
and 2 except `systemd --user`, which the kernel does not allow to be moved. The
VirtualBox VM was saved for the whole study.

Launch scores are medians of five repetitions; the table gives the median of
the 16 launch scores of each condition.

| Orders | Offset | Unfixed | Fixed | Fixed / unfixed |
|---:|---:|---:|---:|---:|
| 10,000 | 0 | 6.382 ms | 6.417 ms | +0.5% |
| 10,000 | 16 | 4.962 ms | 4.956 ms | −0.1% |
| 50,000 | 0 | 32.236 ms | 32.438 ms | +0.6% |
| 50,000 | 16 | 24.847 ms | 24.779 ms | −0.3% |
| 100,000 | 0 | 64.208 ms | 64.439 ms | +0.4% |
| 100,000 | 16 | 49.900 ms | 49.533 ms | −0.7% |

The differences are below the launch-to-launch variation of every condition
(CV of the launch scores 2.3% to 5.3%), and the ranges of fixed and unfixed
launches overlap in all six rows. The fix does not change throughput by an
amount this study can resolve.

## Validity

All 64 launches had 0 thermal throttle events, 0 device interrupts on the
measurement cores and 0 s suspended. Other tasks used a median of 0.52 to
0.61 ms of CPU time per second on the measurement cores. The slowest launch,
fixed source at offset 0 in session 1, block 2, took 71.96 ms at 100,000
orders, 4.9 ms above the next slowest; it also had the most foreign CPU time,
7.37 ms per second. It is included in the table.

## Reproduce

```bash
cmake --build build/release -j"$(nproc)"
# The fixed placement build, from the fix commit
python3 scripts/diagnostics/build_mutex_layout.py --output build/mutex-layout
# The unfixed placement build, from a worktree of the parent source
git worktree add --detach ../mm-unfixed 4408236
python3 ../mm-unfixed/scripts/diagnostics/build_mutex_layout.py \
    --build build/release --output build/mutex-layout-unfixed
python3 scripts/bench_diagnose.py --matrix scripts/diagnostics/lost_wakeup_matrix.json \
    --output <directory> --sessions 2 --blocks 8 --repetitions 5 \
    --interference cleared --allow-unmovable systemd
python3 scripts/diagnostics/summarize.py <directory>

# The hang
git show 4408236:benchmarks/bench_baseline_mutex.cpp > /tmp/unfixed.cpp
scripts/diagnostics/build_mutex_shutdown_stress.sh /tmp/unfixed.cpp /tmp/stress-unfixed
scripts/diagnostics/build_mutex_shutdown_stress.sh benchmarks/bench_baseline_mutex.cpp /tmp/stress-fixed
taskset -c 1,3 /tmp/stress-unfixed 10 120
taskset -c 1,3 /tmp/stress-fixed 10 300
```

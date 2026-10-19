# What moves the two-thread benchmarks between launches

Recorded on 2026-10-19 on the 11th Gen Intel Core i5-1135G7, on branch
`bench/variance-sources`. In the [control of 2026-10-15](../2026-10-15_16-04-12_40a57bc_control/REPORT.md),
the SPSC pipeline's throughput at 10,000 orders, both latency reports and the
producer-to-consumer benchmarks of `bench_spsc_ring_buffer` moved between
launches of unchanged binaries. This study finds the cause of each and sorts it
into a defect of the benchmark, which a later pull request fixes, or a property
of the code under test, which is a result.

A **report** is one latency report, printed once per repetition. **Queue wait**
runs from the producer's timestamp to the consumer taking the order out;
**service time** from there to the end of matching; the reported latency is
their sum. A **region** is one of the memory regions a diagnostic program maps at
start-up: a queue placed in two regions differs only in where it lands in memory.

## How it was recorded

Six recordings, one per subdirectory, made with `scripts/record/bench_diagnose.py`
in the `cleared` condition (`systemd --user` allowed to stay), 2 sessions × 8
blocks, 16 launches per condition, 5 repetitions, with the VirtualBox VM saved.
The diagnostic programs are built by `scripts/diagnostics/build_variance_diagnostics.py`
from the unchanged sources; every added line ends in `// DIAG`. All 209 complete
launches had 0 thermal throttle events, 0 s suspended and 0 device interrupts on
the measured cores. `warmup-hung/` is a first attempt at `warmup/` that stopped
on a hung launch; a second attempt, stopped by hand to change how the runner was
started, is not kept.

## Findings

**1. Pipeline throughput, 10,000 orders (`warmup/`).** The first repetition of a
launch is 41% slower than the other four and the slowest in 15 of 16 launches.
One warm-up call of the benchmark removes it (1.01 times the others, within-launch
CV 17% → 7%). Pinning the workers (`latency-split/`) does not. A longer warm-up
hangs at 50,000 orders: Google Benchmark judges this benchmark by the main
thread's CPU time, which is nearly zero while it waits in `join`, extends the
warm-up to 30 iterations on one engine, and the engine's memory pool, which never
takes leaked orders back (AUDIT §5), runs out (`warmup-hung/`).

**2. Latency reports (`latency-split/`).** Across reports, end-to-end latency minus
queue wait stays within about 100 TSC ticks of its median, so all movement of P50
and P99 is queue wait (AUDIT C1). Service time is steady: P50 of 460 ticks and
P99 of 1,360 for SPSC (launch CV 2–3%), 575 and 1,860 for the mutex pipeline
(0.4–1%). Every SPSC report whose maximum exceeds 4.0M ticks (10 of 80) started
with both workers on one CPU, against none of the other 70; with the workers
pinned, no report of 80 exceeded it. In the mutex pipeline the queue depth follows P50
(Spearman 0.88); the diagnostic slowed its consumer by 2% and raised P50 tenfold,
and 75% of the variance of log P50 lies between the reports of one launch.

**3. Queue benchmarks (`placement/`, `counters-a9ab430/`, `checksum-0ff6394/`).** In the
int64 benchmark the consumer adds every item to a checksum at `rsp+0x48` of a
64-byte aligned frame; the producer reads `N` at `rsp+0x40` for every item, so
each item moves that cache line between the cores. Moving only the checksum to
its own line makes the benchmark 4.5 times faster at 10,000 items and 5.3 times
at 100,000. Without that sharing the ring runs at about 9 or about 50 ns per
item, depending on the memory it lands in: 5 of 16 launches, and about one region
in four inside a launch, are slow. Slow regions have ten times the loads
forwarded from the other core per item (0.52 against 0.05) and three times the
memory-ordering machine clears (0.56 against 0.18). The Order benchmark has no
such shared line; its regions differ with a CV of 11% inside a launch. Of the
variance of the mutex queue over all its regions, the offset of its state inside
a cache line explains half (offset 32 fastest), the region 44% and the launch 6%.

| Benchmark | What moves it | Class | Remedy |
|---|---|---|---|
| Pipeline throughput, 10,000 | First repetition pays a one-time cost | Measurement defect | One warm-up call |
| Latency reports | Queue wait, not latency; SPSC workers sharing a CPU at start | Benchmark defect | Report service time and queue wait apart, open-loop producer; pin workers in-process |
| SPSC int64 queue | Checksum shares the producer's line | Benchmark defect | Accumulate the checksum in the consumer |
| SPSC int64 queue | Fast or slow regime by memory | Property of the ring | Report the distribution over fresh regions |
| SPSC Order queue | Level by memory, one draw per launch | Property; defect in sampling | Fresh region per repetition |
| Mutex queue | Offset of the unaligned state; region | Benchmark defect; property | Align the state; fresh region per repetition |

## Limits

Physical addresses cannot be read without root, so which property of the memory
selects a regime is not resolved. The placement programs hold the queue through a
reference, which leaves their frames 16-byte aligned; there the checksum's line
depended on the stack offset, which inflated the launch-level variation of the
SPSC rows in `placement/` and `counters-a9ab430/`. The latency split adds a TSC read and
four stores per order, which slowed the SPSC pipeline by about 12%.

## Reproduce

```bash
python3 scripts/diagnostics/build_variance_diagnostics.py
mkdir -p build/diagnostics && cc -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared \
  scripts/diagnostics/thread_affinity.c -o build/diagnostics/thread_affinity.so -ldl -pthread
python3 scripts/record/bench_diagnose.py --matrix scripts/diagnostics/variance_<name>_matrix.json \
  --output <directory> --sessions 2 --blocks 8 --repetitions 5 \
  --interference cleared --allow-unmovable systemd
```

`<name>` is `warmup`, `split`, `placement`, `counters` or `checksum`, recorded at
the commit named in each subdirectory's `env.json`.

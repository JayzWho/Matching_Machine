# Two-thread benchmark fixes: validation recording of 2026-10-28

The recording checks branch `bench/two-thread-semantics`: the
producer-to-consumer benchmarks of `bench_spsc_ring_buffer` without the
consumer's shared checksum, each measured on 16 memory regions in both thread
placements per launch; and the pipelines' latency split into queue time and
service time, read with fenced TSC readings in the latency-report benchmarks
only.

## Design

`scripts/record/bench_diagnose.py --matrix scripts/diagnostics/two_thread_semantics_matrix.json --allow-unmovable systemd`:
`cleared` condition, 2 sessions × 8 blocks, so 16 launches per condition,
6 repetitions per launch with the first a warm-up. Every program runs
unfiltered, so the single-threaded benchmarks that precede the two-thread ones
run as they will in a control.

| Condition | Program | Build |
|---|---|---|
| queues | `bench_spsc_ring_buffer` | 988720f, the branch (8ba7901 after it changes report scripts only) |
| spsc-pipeline | `bench_matching_engine` | 988720f |
| mutex-pipeline | `bench_baseline_mutex` | 988720f |
| spsc-pipeline-master | `bench_matching_engine` | bf5f9fe, the source of master b473a6d; the binary recorded under tag `archive/pipeline-fixes-2026-10` |
| mutex-pipeline-master | `bench_baseline_mutex` | bf5f9fe, likewise |

The branch's binaries were copied to `build/variants/c5/`, so that their paths
have the length of master's in `build/variants/c2/`. Machine: i5-1135G7 at its
base frequency with turbo off and the SMT siblings offline, kernel
7.0.0-34-generic, GCC 15.2.0 with `-O2 -DNDEBUG -std=c++20`. Launches ran on
CPUs 1 and 3; placement 0 and both pipelines put the producer on CPU 1. The
VirtualBox VM was saved. All 80 launches had 0 thermal throttle events, 0 s
suspended and 0 device interrupts on the measured cores, and at most 19 ms of
foreign CPU time per second. The TSC calibrated at 2.419182 GHz all four times.

## Hypotheses, fixed before recording

| | Hypothesis | Result |
|---|---|---|
| H1 | int64 ring: two tiers with a gap of at least 1.5× at each size; at 100,000 items, each tier's launch level within a CV of 3% | holds |
| H2 | Slow share 0.15–0.35 in each placement; no region slow in both placements | holds |
| H3 | Order ring and mutex queue: the median over a launch's 32 instances within a CV of 3% | holds except the mutex queue at 10,000 items (CV 3.02%) |
| H4 | Service-time P50 within a CV of 3% in each pipeline | holds |
| H5 | No pipeline throughput more than 2% slower than master | holds |

## Results

**int64 ring**, ns per item. The tiers split at 22.4 ns (100,000 items) and
17.6 ns (10,000 items). A launch's level of a tier is the median of its
instances in that tier; CV is over the 16 launches. At 10,000 items the slow
tier is lower and moves more, presumably because a larger part of each
transfer passes before the consumer has caught up.

| Items | Gap | Fast tier | CV | Slow tier | CV | Slow share, placement 0 / 1 | Slow regions per launch and placement |
|---|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 2.71× | 9.74 | 1.7% | 37.0 | 5.2% | 0.29 / 0.23 | 1 to 7 of 16 |
| 100,000 | 4.40× | 8.09 | 2.6% | 56.8 | 1.7% | 0.29 / 0.23 | 1 to 7 of 16 |

No region was slow in both placements in any launch. In every launch the same
instances are slow at both sizes, which share the 16 regions.

**Order ring and mutex queue**, ns per item, median over a launch's 32
instances. The mutex launches of the second session are about 2% above those
of the first, and the first two Order launches are about 7% below the others.

| Benchmark | Items | Median | CV | Spread | Quartiles over the instances |
|---|---:|---:|---:|---:|---:|
| `BM_SPSC_Order_ProducerConsumer` | 10,000 | 54.7 | 2.8% | 10.7% | 51.3 to 59.8 |
| `BM_Mutex_ProducerConsumer` | 10,000 | 294.9 | 3.0% | 13.8% | 279.9 to 318.7 |
| `BM_Mutex_ProducerConsumer` | 100,000 | 300.4 | 2.4% | 9.3% | 285.5 to 320.0 |

**Pipeline throughput against master**, ms per run, median of launch medians.
p is the share of 20,000 shufflings of the launch medians between the two
conditions that give at least the observed difference.

| Orders | SPSC, master | SPSC, branch | Change (p) | Mutex, master | Mutex, branch | Change (p) |
|---|---:|---:|---:|---:|---:|---:|
| 10,000 | 2.450 | 2.429 | −0.8% (0.77) | 6.421 | 6.513 | +1.4% (0.63) |
| 50,000 | 13.426 | 13.082 | −2.6% (<0.001) | 32.373 | 32.892 | +1.6% (0.02) |
| 100,000 | 28.735 | 28.444 | −1.0% (0.28) | 64.543 | 64.592 | +0.1% (0.98) |

**Latency at 100,000 orders**, TSC ticks, median over launches of a launch's
median report.

| Pipeline | Service P50 (CV) | Service P99 (CV) | Queue P50 (CV) |
|---|---:|---:|---:|
| SPSC | 510 (1.2%) | 1,418 (1.8%) | 3,537,717 (3.2%) |
| Mutex | 616 (0.3%) | 1,888 (1.0%) | 8,309,684 (15.8%) |

The mutex queue is unbounded, so its queue time depends on how much faster the
producer is than the consumer, and it moves between launches.

**The fences slow the latency-report run.** In the full SPSC ring an order
waits for the 4,095 ahead of it, so the median queue time divided by 4,095
estimates the consumer's time per order in that run: 357 ns, against 284 ns per
order in the throughput runs, about 26% slower. The same estimate from master's
end-to-end latency, which is almost all queueing, gives 277 ns against 287 ns.
The queue time reported is therefore that of a slower consumer.

**Tail events.** None of master's 80 counted SPSC reports has a maximum above
4.0M ticks, the mark of a run whose two workers started on one CPU, although
`BM_SingleThread_Baseline` ran before every one of them. On the branch the
slower consumer puts the queue time's typical maximum at 4.09M ticks, and the
highest maximum is 1.02 times that.

## Reproduce

```bash
scripts/diagnostics/analyze_two_thread_semantics.py docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-28_20-55-44_988720f_two-thread-semantics
```

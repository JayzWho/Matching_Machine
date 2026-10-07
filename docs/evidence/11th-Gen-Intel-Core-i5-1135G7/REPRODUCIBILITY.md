# Reproducibility of the control measurement

Three runs of the same unfixed benchmark binaries on this machine, to find out
how far the results move when the code does not change. That distance is the
threshold a later fix has to exceed before a difference can be attributed to
the fix.

> These are control measurements, not performance results. Every number comes
> from benchmark code with the defects documented in `docs/AUDIT.md`.

## What differed between the runs

| | Between A and B | Between B and C |
|---|---|---|
| Time apart | 15 days | 6 minutes |
| Kernel | 7.0.0-31 → 7.0.0-34 | unchanged |
| Reboot in between | yes | no |
| Benchmark binaries | identical files | identical files |
| Measurement cores, frequency policy | identical | identical |

The binaries were built on 2026-09-20 and not rebuilt since. The package log
shows no upgrade of glibc, libstdc++, Abseil or CPU microcode between A and C.
All three runs used cores `3,1` at a pinned 2.4 GHz, and each was preceded by
its own `setup` and followed by `restore`. In every run the thermal throttle
counters did not move and no device interrupt was delivered to a measurement
core. A 60-second load test before run B measured both cores at 2.394 GHz
(`frequency_check/` in that run's directory), the same as before run A.

## Findings

**Single-threaded benchmarks reproduce.** Between B and C the median absolute
change is 0.56% and the largest is 2.06%. Across the kernel update and reboot,
A to B, the median is 0.58%. One benchmark is an exception:
`BM_SPSC_SingleThread_Push`, which takes about one nanosecond per iteration,
went from 1.4 ns to 1.1 ns between A and B and then stayed there in C. The
shift arrived with the reboot or the kernel and persisted; its cause was not
investigated.

**Two-threaded benchmarks do not reproduce.** Between B and C, with nothing
changed, the mutex pipeline became 31% to 36% slower at all three sizes and the
SPSC pipeline 4% to 10% slower. The differences between A and B are of a
comparable size, so they cannot be attributed to the kernel update.

**The variation within a run hides this.** The coefficient of variation (CV,
the standard deviation of a run's ten repetitions divided by their mean) of
the two-threaded pipeline benchmarks is 2% to 4% at the 50,000 and 100,000
order sizes, while the same benchmarks differ by up to 34% from one run to
the next. The repetitions inside one run agree with each other and still do
not predict the next run. A within-run CV therefore cannot be used as the
uncertainty of a before/after comparison.

**The headline ratio is not stable either.** The throughput of the SPSC
pipeline relative to the mutex baseline at 100,000 orders came out as 1.70×,
1.81× and 2.17× in the three runs, from identical code.

## What this means for later comparisons

- For single-threaded benchmarks a stored control run is usable. Leaving aside
  the one exception above, the largest change seen without a code change is
  2.1% within one boot (B to C) and 3.1% across the kernel update (A to C), so
  a difference smaller than about 4% should not be attributed to a code
  change. This rests on three runs and should be revised as more accumulate.
- For two-threaded benchmarks a stored control run is not usable. A fix
  cannot be evaluated against run A, B or C; the comparison has to be made
  within one session, and the instability itself has to be reduced first.

## What is and is not known about the cause

Ruled out by the data above: the kernel version, the core frequency, thermal
throttling, device interrupts, and the choice of cores.

Not controlled, and therefore candidates rather than findings:

- Which thread runs on which core. Affinity is set for the whole process with
  `taskset -c 1,3`; the scheduler decides where the producer and the consumer
  each run, and can move them.
- Processor idle states (C-states, the low-power states a core enters when it
  has nothing to run, which take time to leave). `bench_env.sh` does not
  restrict them, and the mutex pipeline's consumer sleeps whenever its queue is
  empty.
- Whether the state that differs is set per session or per process. Each
  benchmark binary runs once per session, so the three runs cannot tell these
  apart.

## Data

Generated from the raw files of the three runs by:

```
cd docs/evidence/11th-Gen-Intel-Core-i5-1135G7
../../../scripts/compare_runs.py \
    2026-09-22_16-04-18_575ea6e_v1-asis \
    2026-10-07_12-46-57_54a8cfe_v1-asis-kernel-7.0.0-34 \
    2026-10-07_12-53-00_54a8cfe_v1-asis-kernel-7.0.0-34-repeat
```

Times are real time, the median of ten repetitions. "Largest within-run CV" is
the largest of the three runs' CVs for that benchmark. Latency is in TSC ticks,
the unit of the processor's timestamp counter; it has not been converted to
time because the counter's frequency has not been calibrated yet.

### Runs

| Run | Directory | Recorded | Commit | Kernel | Cores | Verdict | Throttle delta |
|---|---|---|---|---|---|---|---|
| A | `2026-09-22_16-04-18_575ea6e_v1-asis` | 2026-09-22 16:04 | `575ea6e` | 7.0.0-31-generic | 3,1 | VALID | core +0, package +0 |
| B | `2026-10-07_12-46-57_54a8cfe_v1-asis-kernel-7.0.0-34` | 2026-10-07 12:46 | `54a8cfe` | 7.0.0-34-generic | 3,1 | VALID | core +0, package +0 |
| C | `2026-10-07_12-53-00_54a8cfe_v1-asis-kernel-7.0.0-34-repeat` | 2026-10-07 12:53 | `54a8cfe` | 7.0.0-34-generic | 3,1 | VALID | core +0, package +0 |

### Single-threaded benchmarks

| Benchmark | Unit | A median | B median | B vs A | C median | C vs A | C vs B | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `BM_AddOrder_NoMatch` | ns | 39.5 | 39.1 | -1.0% | 38.3 | -3.1% | -2.1% | 0.99% |
| `BM_AddOrder_FullMatch` | ns | 405.4 | 403.5 | -0.5% | 403.4 | -0.5% | 0.0% | 0.25% |
| `BM_AddOrder_SweepLevels/1` | ns | 449.7 | 452.3 | +0.6% | 448.7 | -0.2% | -0.8% | 0.13% |
| `BM_AddOrder_SweepLevels/5` | ns | 648.2 | 648.3 | 0.0% | 649.0 | +0.1% | +0.1% | 0.97% |
| `BM_AddOrder_SweepLevels/10` | ns | 884.3 | 883.8 | -0.1% | 890.7 | +0.7% | +0.8% | 4.30% |
| `BM_AddOrder_SweepLevels/20` | ns | 1,660.9 | 1,637.5 | -1.4% | 1,668.7 | +0.5% | +1.9% | 4.53% |
| `BM_CancelOrder` | ns | 44,996.1 | 44,708.1 | -0.6% | 44,433.9 | -1.2% | -0.6% | 0.64% |
| `BM_MixedWorkload/iterations:50` | ns | 134,870,799 | 133,300,902 | -1.2% | 133,102,078 | -1.3% | -0.1% | 1.51% |
| `BM_SPSC_SingleThread_Push` | ns | 1.4 | 1.1 | -21.7% | 1.1 | -21.2% | +0.6% | 1.86% |
| `BM_SPSC_SingleThread_PushPop` | ns | 1.6 | 1.6 | -0.3% | 1.6 | -0.1% | +0.2% | 0.36% |
| `BM_SingleThread_Baseline` | us | 1,402.8 | 1,399.5 | -0.2% | 1,403.7 | +0.1% | +0.3% | 0.27% |

### Two-threaded benchmarks

| Benchmark | Unit | A median | B median | B vs A | C median | C vs A | C vs B | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `BM_SPSC_ProducerConsumer/10000/real_time` | ns | 379,196 | 331,865 | -12.5% | 313,339 | -17.4% | -5.6% | 12.12% |
| `BM_SPSC_ProducerConsumer/100000/real_time` | ns | 4,774,601 | 3,706,400 | -22.4% | 3,939,327 | -17.5% | +6.3% | 4.77% |
| `BM_Mutex_ProducerConsumer/10000/real_time` | ns | 3,508,658 | 2,969,935 | -15.4% | 2,961,405 | -15.6% | -0.3% | 11.22% |
| `BM_Mutex_ProducerConsumer/100000/real_time` | ns | 33,720,443 | 29,733,045 | -11.8% | 28,691,552 | -14.9% | -3.5% | 3.27% |
| `BM_SPSC_Order_ProducerConsumer/10000/real_time` | ns | 500,552 | 578,890 | +15.7% | 583,107 | +16.5% | +0.7% | 1.33% |
| `BM_Pipeline_Throughput/10000/iterations:3` | ms | 2.5 | 2.3 | -9.3% | 2.4 | -6.0% | +3.6% | 13.83% |
| `BM_Pipeline_Throughput/50000/iterations:3` | ms | 13.5 | 12.6 | -6.7% | 13.8 | +2.5% | +9.8% | 2.29% |
| `BM_Pipeline_Throughput/100000/iterations:3` | ms | 29.1 | 26.9 | -7.6% | 29.3 | +0.7% | +9.0% | 2.35% |
| `BM_Pipeline_LatencyReport/iterations:1` | ms | 5.3 | 5.4 | +1.1% | 5.5 | +2.9% | +1.8% | 5.65% |
| `BM_MutexPipeline_Throughput/10000/iterations:3` | ms | 5.0 | 4.7 | -6.6% | 6.4 | +27.5% | +36.5% | 5.84% |
| `BM_MutexPipeline_Throughput/50000/iterations:3` | ms | 24.9 | 24.1 | -3.4% | 32.3 | +29.5% | +34.0% | 3.51% |
| `BM_MutexPipeline_Throughput/100000/iterations:3` | ms | 49.4 | 48.7 | -1.4% | 63.6 | +28.8% | +30.7% | 2.14% |
| `BM_MutexPipeline_LatencyReport/iterations:1` | ms | 5.3 | 5.2 | -1.2% | 5.4 | +2.9% | +4.1% | 7.97% |

### Size of the change between runs

| Group | Comparison | Benchmarks | Median absolute change | Largest absolute change |
|---|---|---:|---:|---:|
| Single-threaded | B vs A | 11 | 0.58% | 21.68% |
| Single-threaded | C vs A | 11 | 0.51% | 21.24% |
| Single-threaded | C vs B | 11 | 0.56% | 2.06% |
| Two-threaded | B vs A | 13 | 7.64% | 22.37% |
| Two-threaded | C vs A | 13 | 15.60% | 29.48% |
| Two-threaded | C vs B | 13 | 5.58% | 36.47% |

### Pipeline throughput at 100,000 orders (from real time)

| Run | SPSC pipeline | Mutex baseline | SPSC / mutex |
|---|---:|---:|---:|
| A | 3.43 M orders/s | 2.02 M orders/s | 1.70× |
| B | 3.72 M orders/s | 2.05 M orders/s | 1.81× |
| C | 3.41 M orders/s | 1.57 M orders/s | 2.17× |

### End-to-end latency at 100,000 orders (TSC ticks; median of the run's reports)

| Run | SPSC P50 | SPSC P99 | Mutex P50 | Mutex P50 range | Mutex P99 |
|---|---:|---:|---:|---:|---:|
| A | 2,672,502 | 3,354,722 | 657,466 | 183,911–5,206,257 | 3,215,196 |
| B | 2,793,502 | 3,461,222 | 1,008,386 | 228,304–2,163,491 | 4,346,098 |
| C | 2,697,541 | 3,403,975 | 677,398 | 150,948–1,441,633 | 3,115,500 |


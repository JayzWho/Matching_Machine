# Reproducibility of the control measurement

Three runs of the same unfixed benchmark binaries on this machine, to find out
how far the results move when the code does not change. That distance is the
threshold a later fix has to exceed before a difference can be attributed to
the fix.

> These are control measurements, not performance results. Every number comes
> from benchmark code with the defects documented in `docs/AUDIT.md`.

## The three runs

| | Run A | Run B | Run C |
|---|---|---|---|
| Recorded | 2026-09-22 16:04 | 2026-10-07 12:46 | 2026-10-07 12:53 |
| Kernel | 7.0.0-31 | 7.0.0-34 | 7.0.0-34 |
| Boot | an earlier boot | same boot as C | same boot as B |
| Benchmark binaries | built 2026-09-20 | the same files | the same files |
| Measurement cores | 3,1 | 3,1 | 3,1 |
| Frequency policy | pinned 2.4 GHz | pinned 2.4 GHz | pinned 2.4 GHz |
| Thermal throttle events during the run | 0 | 0 | 0 |
| Device interrupts on a measurement core during the run | 0 | 0 | 0 |

A and B are separated by fifteen days, a kernel update and a reboot. B and C
are separated by six minutes and nothing else. The package log shows no
upgrade of glibc, libstdc++, Abseil or CPU microcode between A and C. Each run
was preceded by its own `setup` and followed by `restore`. A 60-second load
test before run B measured both cores at 2.394 GHz (`frequency_check/` in that
run's directory), the same as before run A.

## How to read the tables

The benchmarks are grouped into six families that share a workload, plus the
latency percentiles of the two pipelines. For each family there are two tables
under [Data](#data).

The first gives the median real time of every benchmark in every run. "Spread"
is the largest of the three medians relative to the smallest. "Largest
within-run CV" is the largest coefficient of variation (CV, the standard
deviation of a run's ten repetitions divided by their mean) that the benchmark
had in any one run.

The second is a matrix with one row and one column per run. A cell off the
diagonal is the change of the row's run relative to the column's run, as the
range over the family's benchmarks; `C` against `vs B` reading `+30.7% to
+36.5%` means every benchmark of the family took between 30.7% and 36.5%
longer in C than in B. A cell on the diagonal is the largest CV inside that
run. A row therefore shows the spread inside a run next to its distance from
the other two.

`BM_Pipeline_LatencyReport` and `BM_MutexPipeline_LatencyReport` are left out
of the timing tables. Their timed region is the sorting and printing of the
latency report, with the pipeline itself run while the timer is paused; what
they produce is the percentiles, which have their own tables.

## Findings

| Family | Threads | Largest change between A and B | between A and C | between B and C |
|---|---:|---:|---:|---:|
| Order book and matching, 9 benchmarks | 1 | 1.4% | 3.2% | 2.1% |
| Ring buffer push and pop, 2 benchmarks | 1 | 27.7% | 27.0% | 0.6% |
| SPSC ring buffer, producer to consumer, 3 benchmarks | 2 | 28.8% | 21.2% | 6.3% |
| Mutex queue, producer to consumer, 2 benchmarks | 2 | 18.1% | 18.5% | 3.6% |
| SPSC pipeline, 3 benchmarks | 2 | 10.2% | 6.4% | 9.8% |
| Mutex pipeline, 3 benchmarks | 2 | 7.0% | 29.5% | 36.5% |

Each figure is the larger of the two directions in the family's matrix.

**The order book and matching benchmarks reproduce.** All nine are
single-threaded, and no pair of runs differs by more than 3.2% on any of them.

**The ring buffer benchmarks shifted between A and the two later runs, which
agree with each other.** All but one of the seven benchmarks in
`bench_spsc_ring_buffer` moved by 12% to 22% between A and B, and B and C then
agree to within 6.3%. The shift includes a single-threaded benchmark,
`BM_SPSC_SingleThread_Push`, which went from 1.407 ns to 1.102 ns and stayed
there. It is not a uniform speed-up: `BM_SPSC_Order_ProducerConsumer` became
16% slower while the others became faster. The shift coincides with the kernel
update and the reboot, and three runs cannot separate those two or show
whether the next reboot moves the results again.

**The mutex pipeline changed within one boot.** A and B agree to within 7%.
C, recorded six minutes after B with nothing changed, is 31% to 36% slower
than B at all three sizes.

**The SPSC pipeline differs by up to 10% between any two runs**, with no run
standing apart from the other two.

**The spread inside a run does not predict the distance to another run.** The
mutex pipeline at 100,000 orders has a CV of 1.4% to 2.1% in each of the three
runs, and C is 30.7% slower than B. The mutex queue at 100,000 items has a CV
of 1.1% to 3.3%, and A is 17.5% slower than C. A within-run CV therefore
cannot be used as the uncertainty of a before/after comparison.

**The headline ratio is not stable.** The throughput of the SPSC pipeline
relative to the mutex pipeline at 100,000 orders came out as 1.70×, 1.81× and
2.17× in the three runs, from identical code.

**The latency percentiles of the mutex pipeline are not stable inside a run.**
Its P50 ranges from 183,911 to 5,206,257 TSC ticks across the ten reports of
run A, a factor of 28, so the median of ten reports is not a settled quantity
and the differences between runs, up to 53%, are inside that spread. The SPSC
pipeline's median P50 and P99 agree to within 4.5% across the three runs. In
all three runs the SPSC pipeline's median P50 is 2.8 to 4.1 times the mutex
pipeline's, where the README claims it is 3.8 times lower.

## What this means for later comparisons

- **Order book and matching.** A stored control run is usable. The largest
  change seen without a code change is 3.2%, so a difference smaller than
  about 4% should not be attributed to a code change. This rests on three runs
  and should be revised as more accumulate.
- **Ring buffer benchmarks.** A stored control run has held within one boot
  and kernel, on the evidence of one pair of runs, and has not held across a
  kernel update and reboot. Until the cause of the shift is known, the control
  has to be recorded again after every reboot.
- **Pipelines.** A stored control run is not usable. A fix cannot be evaluated
  against run A, B or C; the comparison has to be made within one session, and
  the variation itself has to be reduced first.

## What is and is not known about the cause

The same in all three runs, and therefore not the cause of any difference
between them: the binaries, the measurement cores, the pinned frequency, the
absence of thermal throttling and the absence of device interrupts on the
measurement cores.

The mutex pipeline changed between B and C, which share a kernel and a boot,
so neither explains it. The ring buffer benchmarks changed between A and B,
where the kernel, the boot and fifteen days all differ, so the kernel and the
boot both remain candidates there.

Not controlled, and therefore candidates rather than findings:

- Which thread runs on which core. Affinity is set for the whole process with
  `taskset -c 1,3`; the scheduler decides where the producer and the consumer
  each run, and can move them.
- Processor idle states (C-states, the low-power states a core enters when it
  has nothing to run, which take time to leave). `bench_env.sh` does not
  restrict them, and the mutex pipeline's consumer sleeps whenever its queue is
  empty.
- Whether the state that differs is set per boot, per session or per process.
  Each benchmark binary runs once per session, and only B and C share a boot,
  so the three runs cannot tell these apart.

## Data

Generated from the raw files of the three runs by the command below; the
output follows unchanged except that its headings are one level deeper.

```
cd docs/evidence/11th-Gen-Intel-Core-i5-1135G7
../../../scripts/compare_runs.py \
    2026-09-22_16-04-18_575ea6e_v1-asis \
    2026-10-07_12-46-57_54a8cfe_v1-asis-kernel-7.0.0-34 \
    2026-10-07_12-53-00_54a8cfe_v1-asis-kernel-7.0.0-34-repeat
```

Times are real time, the median of ten repetitions. Latency is in TSC ticks,
the unit of the processor's timestamp counter, as the median of the run's ten
reports; it has not been converted to time because the counter's frequency has
not been calibrated yet. The CV of the latency reports uses the sample
standard deviation, as Google Benchmark does for times; `ANALYSIS.md` in run
A's directory used the population form, which gives 116% where the table here
gives 122%.

### Runs

| Run | Directory | Recorded | Commit | Kernel | Cores | Verdict | Throttle delta |
|---|---|---|---|---|---|---|---|
| A | `2026-09-22_16-04-18_575ea6e_v1-asis` | 2026-09-22 16:04 | `575ea6e` | 7.0.0-31-generic | 3,1 | VALID | core +0, package +0 |
| B | `2026-10-07_12-46-57_54a8cfe_v1-asis-kernel-7.0.0-34` | 2026-10-07 12:46 | `54a8cfe` | 7.0.0-34-generic | 3,1 | VALID | core +0, package +0 |
| C | `2026-10-07_12-53-00_54a8cfe_v1-asis-kernel-7.0.0-34-repeat` | 2026-10-07 12:53 | `54a8cfe` | 7.0.0-34-generic | 3,1 | VALID | core +0, package +0 |

### Measurements

#### Order book and matching (one thread, 9 benchmarks)

| Benchmark | Unit | A | B | C | Spread | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|
| `BM_AddOrder_NoMatch` | ns | 39.50 | 39.10 | 38.29 | 3.2% | 1.0% |
| `BM_AddOrder_FullMatch` | ns | 405.4 | 403.5 | 403.4 | 0.5% | 0.3% |
| `BM_AddOrder_SweepLevels/1` | ns | 449.7 | 452.3 | 448.7 | 0.8% | 0.1% |
| `BM_AddOrder_SweepLevels/5` | ns | 648.2 | 648.3 | 649.0 | 0.1% | 1.0% |
| `BM_AddOrder_SweepLevels/10` | ns | 884.3 | 883.8 | 890.7 | 0.8% | 4.3% |
| `BM_AddOrder_SweepLevels/20` | ns | 1,661 | 1,638 | 1,669 | 1.9% | 4.5% |
| `BM_CancelOrder` | us | 45.00 | 44.71 | 44.43 | 1.3% | 0.6% |
| `BM_MixedWorkload/iterations:50` | ms | 134.9 | 133.3 | 133.1 | 1.3% | 1.5% |
| `BM_SingleThread_Baseline` | us | 1,403 | 1,399 | 1,404 | 0.3% | 0.3% |

#### Ring buffer push and pop (one thread, 2 benchmarks)

| Benchmark | Unit | A | B | C | Spread | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|
| `BM_SPSC_SingleThread_Push` | ns | 1.407 | 1.102 | 1.108 | 27.7% | 1.9% |
| `BM_SPSC_SingleThread_PushPop` | ns | 1.565 | 1.561 | 1.564 | 0.3% | 0.4% |

#### SPSC ring buffer, producer to consumer (two threads, 3 benchmarks)

| Benchmark | Unit | A | B | C | Spread | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|
| `BM_SPSC_ProducerConsumer/10000/real_time` | us | 379.2 | 331.9 | 313.3 | 21.0% | 12.1% |
| `BM_SPSC_ProducerConsumer/100000/real_time` | us | 4,775 | 3,706 | 3,939 | 28.8% | 4.8% |
| `BM_SPSC_Order_ProducerConsumer/10000/real_time` | us | 500.6 | 578.9 | 583.1 | 16.5% | 1.3% |

#### Mutex queue, producer to consumer (two threads, 2 benchmarks)

| Benchmark | Unit | A | B | C | Spread | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|
| `BM_Mutex_ProducerConsumer/10000/real_time` | us | 3,509 | 2,970 | 2,961 | 18.5% | 11.2% |
| `BM_Mutex_ProducerConsumer/100000/real_time` | ms | 33.72 | 29.73 | 28.69 | 17.5% | 3.3% |

#### SPSC pipeline (two threads, 3 benchmarks)

| Benchmark | Unit | A | B | C | Spread | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|
| `BM_Pipeline_Throughput/10000/iterations:3` | us | 2,530 | 2,296 | 2,379 | 10.2% | 13.8% |
| `BM_Pipeline_Throughput/50000/iterations:3` | ms | 13.48 | 12.57 | 13.81 | 9.8% | 2.3% |
| `BM_Pipeline_Throughput/100000/iterations:3` | ms | 29.12 | 26.89 | 29.31 | 9.0% | 2.3% |

#### Mutex pipeline (two threads, 3 benchmarks)

| Benchmark | Unit | A | B | C | Spread | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|
| `BM_MutexPipeline_Throughput/10000/iterations:3` | us | 5,018 | 4,689 | 6,399 | 36.5% | 5.8% |
| `BM_MutexPipeline_Throughput/50000/iterations:3` | ms | 24.91 | 24.06 | 32.25 | 34.0% | 3.5% |
| `BM_MutexPipeline_Throughput/100000/iterations:3` | ms | 49.40 | 48.68 | 63.62 | 30.7% | 2.1% |

#### SPSC pipeline latency at 100,000 orders (P50 and P99)

| Benchmark | Unit | A | B | C | Spread | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|
| `SPSC pipeline P50` | TSC ticks | 2,672,502 | 2,793,502 | 2,697,541 | 4.5% | 6.2% |
| `SPSC pipeline P99` | TSC ticks | 3,354,722 | 3,461,222 | 3,403,975 | 3.2% | 61.8% |

#### Mutex pipeline latency at 100,000 orders (P50 and P99)

| Benchmark | Unit | A | B | C | Spread | Largest within-run CV |
|---|---|---:|---:|---:|---:|---:|
| `Mutex pipeline P50` | TSC ticks | 657,466 | 1,008,386 | 677,398 | 53.4% | 122.4% |
| `Mutex pipeline P99` | TSC ticks | 3,215,196 | 4,346,098 | 3,115,500 | 39.5% | 63.0% |

### Differences between runs

#### Order book and matching (one thread, 9 benchmarks)

| | vs A | vs B | vs C |
|---|---:|---:|---:|
| **A** | *CV 4.5%* | -0.6% to +1.4% | -0.7% to +3.2% |
| **B** | -1.4% to +0.6% | *CV 4.4%* | -1.9% to +2.1% |
| **C** | -3.1% to +0.7% | -2.1% to +1.9% | *CV 4.3%* |

#### Ring buffer push and pop (one thread, 2 benchmarks)

| | vs A | vs B | vs C |
|---|---:|---:|---:|
| **A** | *CV 1.9%* | +0.3% to +27.7% | +0.1% to +27.0% |
| **B** | -21.7% to -0.3% | *CV 0.4%* | -0.6% to -0.2% |
| **C** | -21.2% to -0.1% | +0.2% to +0.6% | *CV 0.3%* |

#### SPSC ring buffer, producer to consumer (two threads, 3 benchmarks)

| | vs A | vs B | vs C |
|---|---:|---:|---:|
| **A** | *CV 7.0%* | -13.5% to +28.8% | -14.2% to +21.2% |
| **B** | -22.4% to +15.7% | *CV 12.1%* | -5.9% to +5.9% |
| **C** | -17.5% to +16.5% | -5.6% to +6.3% | *CV 2.2%* |

#### Mutex queue, producer to consumer (two threads, 2 benchmarks)

| | vs A | vs B | vs C |
|---|---:|---:|---:|
| **A** | *CV 11.2%* | +13.4% to +18.1% | +17.5% to +18.5% |
| **B** | -15.4% to -11.8% | *CV 1.6%* | +0.3% to +3.6% |
| **C** | -15.6% to -14.9% | -3.5% to -0.3% | *CV 1.1%* |

#### SPSC pipeline (two threads, 3 benchmarks)

| | vs A | vs B | vs C |
|---|---:|---:|---:|
| **A** | *CV 12.8%* | +7.2% to +10.2% | -2.4% to +6.4% |
| **B** | -9.3% to -6.7% | *CV 12.3%* | -9.0% to -3.5% |
| **C** | -6.0% to +2.5% | +3.6% to +9.8% | *CV 13.8%* |

#### Mutex pipeline (two threads, 3 benchmarks)

| | vs A | vs B | vs C |
|---|---:|---:|---:|
| **A** | *CV 5.8%* | +1.5% to +7.0% | -22.8% to -21.6% |
| **B** | -6.6% to -1.4% | *CV 2.9%* | -26.7% to -23.5% |
| **C** | +27.5% to +29.5% | +30.7% to +36.5% | *CV 5.7%* |

#### SPSC pipeline latency at 100,000 orders (P50 and P99)

| | vs A | vs B | vs C |
|---|---:|---:|---:|
| **A** | *CV 6.2%* | -4.3% to -3.1% | -1.4% to -0.9% |
| **B** | +3.2% to +4.5% | *CV 61.8%* | +1.7% to +3.6% |
| **C** | +0.9% to +1.5% | -3.4% to -1.7% | *CV 16.1%* |

#### Mutex pipeline latency at 100,000 orders (P50 and P99)

| | vs A | vs B | vs C |
|---|---:|---:|---:|
| **A** | *CV 122.4%* | -34.8% to -26.0% | -2.9% to +3.2% |
| **B** | +35.2% to +53.4% | *CV 59.5%* | +39.5% to +48.9% |
| **C** | -3.1% to +3.0% | -32.8% to -28.3% | *CV 68.2%* |

### Pipeline throughput at 100,000 orders (from real time)

| Run | SPSC pipeline | Mutex pipeline | SPSC / mutex |
|---|---:|---:|---:|
| A | 3.43 M orders/s | 2.02 M orders/s | 1.70× |
| B | 3.72 M orders/s | 2.05 M orders/s | 1.81× |
| C | 3.41 M orders/s | 1.57 M orders/s | 2.17× |


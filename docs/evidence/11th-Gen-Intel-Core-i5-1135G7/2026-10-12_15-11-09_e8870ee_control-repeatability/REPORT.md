# Repeatability of the unfixed benchmarks

Recorded on 2026-10-12 on the 11th Gen Intel Core i5-1135G7 at commit `e8870ee`,
with the binaries built on 2026-09-20.

These are control measurements. The benchmark programs still contain the defects
listed in [`docs/AUDIT.md`](../../../AUDIT.md), so the numbers describe how
repeatable those programs are on this machine and not how fast the engine is.

## Question

Three earlier recordings of the same binaries, each with one launch per binary,
disagreed by up to 36% on some benchmarks. This study asks whether results move
when nothing in the code changes, and between what: repetitions, launches,
sessions, or time.

## How it was recorded

Each binary was launched 32 times within one boot: 4 sessions of 8 rounds, with
5 repetitions of every benchmark per launch. A session is one span of
measurement mode: frequency pinned at 2400 MHz with turbo off, SMT siblings
offline, device interrupts steered to the cores that are not measured. A round
launches each of the four binaries once, in an order that rotates. Rounds
alternate between two conditions. In `shared`, the other processes of the user
may run on the two measurement cores. In `cleared`, every one of them is
restricted to the two other cores. A VirtualBox virtual machine that had been
running during two of the earlier recordings was paused for the whole study.

A launch is represented by the median of its repetitions.

## Findings

**1. Nothing drifts between sessions or over time.** No benchmark differs
between the four sessions: the smallest p among the 44 tests is 0.008, a value
that 44 tests without any effect reach about one time in three. No benchmark
shows a trend over the 73 minutes either (smallest p 0.02).

**2. Results move from one launch to the next.** In 37 of the 44 combinations
of benchmark and condition, launches differ by more than their repetitions
scatter. How much depends on the number of threads:

| Benchmarks | Launch medians within 1% of their benchmark's median | Middle half of the launch medians | Spread of the launch medians |
|---|---:|---:|---:|
| Single-threaded, 11 | 313 of 352 | 0.1% to 1.8% | 0.6% to 9.8% |
| Two-threaded, 11 | 54 of 352 | 3.0% to 36.1% | 8.3% to 169.9% |

A single-threaded launch is usually within 1% of the others, with an occasional
launch that is 2% to 9% slower. For a two-threaded benchmark the middle half of
the launches already spans 3% to 36%. The variation inside a launch shows
neither: its coefficient of variation is at most 0.5% for ten of the eleven
single-threaded benchmarks and at most 5.2% for ten of the eleven two-threaded
ones.

**3. The mutex pipeline runs at one of two levels, chosen when the process
starts.** At 100,000 orders, 19 launches took 47.1 to 51.1 ms and 12 took 63.1
to 68.5 ms, with nothing in between (see "Launch medians round by round"). The
remaining launch, at 62.0 ms, belongs to the disturbed round described under
"What was excluded" and cannot be assigned to a level. In the other 31 launches
the three order counts are at the same level, and so are the five repetitions.
The level follows neither the session, nor the time, nor the condition: the
upper level occurred in 5 `shared` and 7 `cleared` rounds.

The SPSC pipeline has no such levels: outside the disturbed round its medians
at 100,000 orders lie between 27.4 and 29.9 ms. The throughput ratio of the two
pipelines therefore depends on the level at which the mutex pipeline happens to
start: 1.60× to 1.80× in the 19 rounds at the lower level and 2.20× to 2.49× in
the 12 rounds at the upper level.

**4. The two producer-to-consumer families move as much, without distinct
levels.** The launch medians of `BM_SPSC_ProducerConsumer/100000` range from
3.68 to 9.47 ms and those of `BM_Mutex_ProducerConsumer/100000` from 23.6 to
42.9 ms, while the repetitions inside a launch scatter by 1% to 3%.

**5. What sets the level of a launch is not identified.** It is fixed for the
life of a process, and it is none of the disturbances in the next section. Of
the recorded quantities, the one that follows the mutex pipeline's level most
closely is its own CPU time per wall time, about 1.76 at the lower level and
1.82 at the upper, which is a property of the level and not a cause recorded
from outside. Two things that differ between launches were not varied here:
the scheduler places the two threads of a benchmark on the two cores as it sees
fit, and the kernel gives every process a different address-space layout.

## What was excluded

| Disturbance | Check | Result |
|---|---|---|
| Thermal throttling | Throttle counters before and after every launch | 0 events in 128 launches, package at 42 to 55 °C |
| Frequency changes | Frequency policy before and after every session, and the frequency of the launch's cores once a second | Pinned at 2400 MHz with turbo off in all four sessions. 2400 MHz in every reading of the 32 launches that keep their core busy throughout |
| Device interrupts | `/proc/interrupts` for the measurement cores | 0 in 128 launches |
| Suspend | `CLOCK_BOOTTIME` against `CLOCK_MONOTONIC` | 0 s |
| Other processes | CPU time on the launch's cores that was not the launch's own | Below |

In `shared`, other tasks used a median of 5.1 ms of CPU time per second on the
launch's cores. In one round of 16 (session 3, round 5) they took 617 and 459
ms per second during two consecutive launches, and `BM_Pipeline_Throughput/100000`
took 38.4 ms in that launch against 27.4 to 29.9 ms in the 31 others. In
`cleared`, the median was 0.8 ms per second, the maximum 5.0, and no launch was
disturbed. The two conditions otherwise give the same results: no benchmark's
median differs between them (smallest p 0.01 among 22 tests).

Competition from other processes therefore exists, and `cleared` removes it,
but it is rare and it is not what moves results from launch to launch.

The timer tick, softirqs and per-CPU kernel threads stayed on the measurement
cores in both conditions.

## The earlier single launches

The medians of the three earlier recordings, next to the lowest and the highest
launch median of this study. Values outside that range are bold. The earlier
recordings had 10 repetitions per benchmark; their files are kept at the tag
`archive/control-runs-2026-10`.

| Benchmark | 2026-09-22 | 2026-10-07, first | 2026-10-07, second | This study: lowest launch | Highest launch |
|---|---:|---:|---:|---:|---:|
| `BM_AddOrder_NoMatch`, ns | 39.50 | 39.10 | 38.29 | 37.15 | 39.82 |
| `BM_AddOrder_FullMatch`, ns | 405.4 | 403.5 | 403.4 | 400.8 | 413.9 |
| `BM_AddOrder_SweepLevels/1`, ns | 449.7 | 452.3 | **448.7** | 448.7 | 457.8 |
| `BM_AddOrder_SweepLevels/5`, ns | 648.2 | 648.3 | 649.0 | 645.3 | 666.1 |
| `BM_AddOrder_SweepLevels/10`, ns | 884.3 | 883.8 | 890.7 | 880.0 | 924.4 |
| `BM_AddOrder_SweepLevels/20`, ns | **1,661** | 1,638 | **1,669** | 1,583 | 1,657 |
| `BM_CancelOrder`, us | 45.00 | 44.71 | 44.43 | 44.24 | 45.24 |
| `BM_MixedWorkload/iterations:50`, ms | 134.9 | **133.3** | **133.1** | 133.4 | 136.1 |
| `BM_SPSC_SingleThread_Push`, ns | **1.407** | 1.102 | 1.108 | 1.098 | 1.205 |
| `BM_SPSC_SingleThread_PushPop`, ns | 1.565 | **1.561** | 1.564 | 1.561 | 1.576 |
| `BM_SPSC_ProducerConsumer/10000/real_time`, us | 379.2 | 331.9 | 313.3 | 290.6 | 808.1 |
| `BM_SPSC_ProducerConsumer/100000/real_time`, us | 4,775 | 3,706 | 3,939 | 3,677 | 9,465 |
| `BM_Mutex_ProducerConsumer/10000/real_time`, us | 3,509 | 2,970 | 2,961 | 2,506 | 4,108 |
| `BM_Mutex_ProducerConsumer/100000/real_time`, ms | 33.72 | 29.73 | 28.69 | 23.58 | 42.90 |
| `BM_SPSC_Order_ProducerConsumer/10000/real_time`, us | 500.6 | 578.9 | 583.1 | 454.0 | 670.9 |
| `BM_Pipeline_Throughput/10000/iterations:3`, us | 2,530 | **2,296** | 2,379 | 2,307 | 3,186 |
| `BM_Pipeline_Throughput/50000/iterations:3`, ms | 13.48 | **12.57** | 13.81 | 12.71 | 14.32 |
| `BM_Pipeline_Throughput/100000/iterations:3`, ms | 29.12 | **26.89** | 29.31 | 27.37 | 38.41 |
| `BM_SingleThread_Baseline`, us | 1,403 | 1,399 | 1,404 | 1,393 | 1,425 |
| `BM_MutexPipeline_Throughput/10000/iterations:3`, us | 5,018 | 4,689 | 6,399 | 4,559 | 6,916 |
| `BM_MutexPipeline_Throughput/50000/iterations:3`, ms | 24.91 | 24.06 | 32.25 | 23.66 | 36.11 |
| `BM_MutexPipeline_Throughput/100000/iterations:3`, ms | 49.40 | 48.68 | 63.62 | 47.12 | 68.46 |

Of the 66 earlier medians, 56 lie inside the range of this study and 9 lie
outside it by less than 2%. One lies 16.8% above it: `BM_SPSC_SingleThread_Push`
on 2026-09-22, which was recorded in an earlier boot under kernel 7.0.0-31.
This study cannot tell whether that value was a rare slow launch or a
difference between boots.

The differences that had been read as drift between those recordings are the
launch-to-launch movement of findings 2 to 4. The mutex pipeline at 100,000
orders, 31% slower in the second recording of 2026-10-07 than in the first,
was once at its upper level (63.62 ms) and once at its lower (48.68 ms).

## Consequences for later measurements

- One launch per binary is not a baseline. A comparison between two versions of
  the code needs many launches of each, and compares their launch medians.
- For the two-threaded benchmarks, a difference of less than tens of percent
  cannot be attributed to a code change until what sets the level of a launch
  is found and controlled.
- A ratio between the two pipelines taken from single launches can come out
  anywhere between 1.6× and 2.5× on this machine.

## Limits

- One boot under kernel 7.0.0-34. The study says nothing about reboots or
  kernel updates.
- 16 launches per condition. A level that occurs less often than about once in
  ten launches can be missed.
- Tasks of other users and kernel threads were measured, not excluded.
- The latency reports are listed in the last table and not interpreted. The
  median latency of the mutex pipeline differs by more than ten times between
  launches.

# Tables

Everything from here on is the unedited output of
`scripts/bench_report.py` for this directory.

## Design

|  |  |
|---|---|
| Label | control-repeatability |
| Commit | `e8870ee` |
| Measurement mode | yes |
| Kernel | 7.0.0-34-generic |
| Measurement cores | 3, 1 (single-threaded binaries on 3) |
| Recorded | 2026-10-12 15:11 to 16:24 |
| Sessions × rounds × repetitions | 4 × 8 × 5 |
| Launches of each binary | 16 shared, 16 cleared |
| Binaries built | 2026-09-20 |

## Validity and interference

| Condition | Launches | Thermal throttle events | Seconds suspended | Device interrupts | Thermal interrupts | MHz, launches on one core | MHz, launches on two cores | Package °C | Foreign CPU ms per s, median / max | Launches above 50 ms per s | Run-queue wait ms per s, median / max | User tasks moved off the cores |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| shared | 64 | 0 | 0 | 0 | 0 | 2400 to 2400 | 1008 to 2400 | 43 to 55 | 5.13 / 617.33 | 2 | 3.39 / 714.56 | 0 |
| cleared | 64 | 0 | 0 | 0 | 0 | 2400 to 2400 | 1120 to 2400 | 42 to 54 | 0.81 / 5.04 | 0 | 2.70 / 23.01 | 666 |

Foreign CPU is time that tasks other than the benchmark ran on the launch's cores. A frequency reading means something only while the core is busy: a launch on one core keeps it busy throughout, while a launch on two cores leaves one idle during its single-threaded benchmarks, and the reading does not say which core it came from.

## Summary by family

How far launches are from each other, as the range over the benchmarks of a family of two measures: the middle half of the launch medians (the distance between their quartiles, relative to their median) and their spread (the largest relative to the smallest, minus one). Within 1% counts the launch medians that are within 1% of the median of their benchmark. Moving counts the benchmarks whose launches differ by more than their repetitions scatter (p < 0.001).

| Family | Threads | Middle half, shared | Spread, shared | Within 1%, shared | Moving, shared | Middle half, cleared | Spread, cleared | Within 1%, cleared | Moving, cleared |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Order book and matching | 1 | 0.2% to 1.8% | 1.2% to 6.5% | 126 of 144 | 8 of 9 | 0.1% to 1.7% | 0.6% to 4.9% | 125 of 144 | 8 of 9 |
| Ring buffer push and pop | 1 | 0.3% to 0.6% | 0.9% to 9.8% | 31 of 32 | 1 of 2 | 0.2% to 0.4% | 0.6% to 1.6% | 31 of 32 | 1 of 2 |
| SPSC ring buffer, producer to consumer | 2 | 18.7% to 24.2% | 47.7% to 92.2% | 5 of 48 | 3 of 3 | 10.7% to 18.4% | 33.0% to 169.9% | 4 of 48 | 3 of 3 |
| Mutex queue, producer to consumer | 2 | 14.0% to 18.7% | 40.6% to 46.9% | 4 of 32 | 2 of 2 | 16.2% to 20.3% | 63.9% to 82.0% | 5 of 32 | 2 of 2 |
| SPSC pipeline | 2 | 3.0% to 6.1% | 10.0% to 40.3% | 9 of 48 | 1 of 3 | 3.0% to 6.0% | 8.3% to 20.9% | 12 of 48 | 2 of 3 |
| Mutex pipeline | 2 | 25.9% to 36.1% | 44.2% to 52.1% | 9 of 48 | 3 of 3 | 33.0% to 35.1% | 41.4% to 46.8% | 6 of 48 | 3 of 3 |

## Launch medians by benchmark

The middle half is the distance between the quartiles of the launch medians, relative to their median. A launch is beyond scatter when its median is farther from the median of all launches than the spread expected from scatter. The three p columns test for differences between launches, between sessions, and for a trend over the recording; values below 0.001 are bold.

### Condition: shared

**Order book and matching** (one thread)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_AddOrder_NoMatch` | 37.77 ns | 0.5% | 1.8% | 6.5% | 1.2% | 4 of 16 | **<0.00005** | 0.88 | 0.99 |
| `BM_AddOrder_FullMatch` | 403.3 ns | 0.1% | 0.4% | 3.1% | 0.2% | 8 of 16 | **<0.00005** | 0.78 | 1.00 |
| `BM_AddOrder_SweepLevels/1` | 450.0 ns | 0.2% | 0.2% | 1.9% | 0.4% | 1 of 16 | **<0.00005** | 0.75 | 0.52 |
| `BM_AddOrder_SweepLevels/5` | 647.8 ns | 0.1% | 0.2% | 1.4% | 0.3% | 2 of 16 | **<0.00005** | 0.92 | 0.79 |
| `BM_AddOrder_SweepLevels/10` | 884.6 ns | 0.1% | 0.5% | 4.9% | 0.3% | 6 of 16 | **<0.00005** | 0.78 | 0.31 |
| `BM_AddOrder_SweepLevels/20` | 1,621 ns | 2.0% | 0.9% | 3.2% | 4.7% | 0 of 16 | 0.42 | 0.21 | 0.02 |
| `BM_CancelOrder` | 44.44 us | 0.3% | 0.3% | 1.2% | 0.9% | 1 of 16 | **<0.00005** | 0.75 | 0.98 |
| `BM_MixedWorkload/iterations:50` | 134.0 ms | 0.1% | 0.4% | 1.7% | 0.4% | 3 of 16 | **<0.00005** | 0.56 | 0.35 |
| `BM_SingleThread_Baseline` | 1,397 us | 0.1% | 0.3% | 2.3% | 0.4% | 2 of 16 | **<0.00005** | 0.40 | 0.74 |

**Ring buffer push and pop** (one thread)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_SPSC_SingleThread_Push` | 1.104 ns | 0.1% | 0.6% | 9.8% | 0.4% | 7 of 16 | **<0.00005** | 0.26 | 0.05 |
| `BM_SPSC_SingleThread_PushPop` | 1.566 ns | 0.3% | 0.3% | 0.9% | 0.7% | 0 of 16 | 0.03 | 0.45 | 0.18 |

**SPSC ring buffer, producer to consumer** (two threads)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_SPSC_ProducerConsumer/10000/real_time` | 472.6 us | 2.7% | 18.7% | 92.2% | 11.2% | 6 of 16 | **<0.00005** | 0.83 | 0.46 |
| `BM_SPSC_ProducerConsumer/100000/real_time` | 5,860 us | 3.3% | 24.2% | 62.8% | 9.0% | 10 of 16 | **<0.00005** | 0.34 | 0.99 |
| `BM_SPSC_Order_ProducerConsumer/10000/real_time` | 525.8 us | 1.1% | 23.2% | 47.7% | 4.2% | 9 of 16 | **<0.00005** | 0.63 | 0.96 |

**Mutex queue, producer to consumer** (two threads)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_Mutex_ProducerConsumer/10000/real_time` | 3,030 us | 1.1% | 14.0% | 40.6% | 3.5% | 16 of 16 | **<0.00005** | 0.42 | 0.93 |
| `BM_Mutex_ProducerConsumer/100000/real_time` | 28.49 ms | 1.2% | 18.7% | 46.9% | 3.8% | 10 of 16 | **<0.00005** | 0.37 | 0.35 |

**SPSC pipeline** (two threads)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_Pipeline_Throughput/10000/iterations:3` | 2,575 us | 18.4% | 6.1% | 37.4% | 57.9% | 0 of 16 | 0.91 | 0.92 | 0.58 |
| `BM_Pipeline_Throughput/50000/iterations:3` | 13.54 ms | 2.8% | 3.9% | 10.0% | 8.5% | 0 of 16 | 0.0012 | 0.06 | 0.14 |
| `BM_Pipeline_Throughput/100000/iterations:3` | 28.76 ms | 2.0% | 3.0% | 40.3% | 5.6% | 1 of 16 | **<0.00005** | 0.20 | 0.24 |

**Mutex pipeline** (two threads)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_MutexPipeline_Throughput/10000/iterations:3` | 5,061 us | 3.8% | 25.9% | 51.7% | 11.4% | 6 of 16 | **<0.00005** | 0.70 | 0.65 |
| `BM_MutexPipeline_Throughput/50000/iterations:3` | 25.18 ms | 2.8% | 36.1% | 52.1% | 7.5% | 6 of 16 | **<0.00005** | 0.96 | 0.84 |
| `BM_MutexPipeline_Throughput/100000/iterations:3` | 49.90 ms | 1.9% | 33.4% | 44.2% | 5.5% | 8 of 16 | **<0.00005** | 0.87 | 0.54 |

### Condition: cleared

**Order book and matching** (one thread)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_AddOrder_NoMatch` | 37.82 ns | 0.2% | 1.0% | 4.9% | 0.6% | 6 of 16 | **<0.00005** | 0.10 | 0.20 |
| `BM_AddOrder_FullMatch` | 403.0 ns | 0.1% | 0.2% | 1.0% | 0.2% | 6 of 16 | **<0.00005** | 0.01 | 0.51 |
| `BM_AddOrder_SweepLevels/1` | 449.7 ns | 0.1% | 0.1% | 2.0% | 0.5% | 2 of 16 | **<0.00005** | 0.76 | 0.96 |
| `BM_AddOrder_SweepLevels/5` | 647.6 ns | 0.1% | 0.4% | 3.2% | 0.3% | 5 of 16 | **<0.00005** | 0.25 | 0.75 |
| `BM_AddOrder_SweepLevels/10` | 883.2 ns | 0.1% | 0.8% | 3.4% | 0.4% | 6 of 16 | **<0.00005** | 0.25 | 0.83 |
| `BM_AddOrder_SweepLevels/20` | 1,615 ns | 2.0% | 1.7% | 4.7% | 5.1% | 0 of 16 | 0.22 | 0.13 | 0.33 |
| `BM_CancelOrder` | 44.45 us | 0.3% | 0.4% | 2.2% | 0.7% | 2 of 16 | **<0.00005** | 0.84 | 0.64 |
| `BM_MixedWorkload/iterations:50` | 134.0 ms | 0.2% | 0.4% | 2.0% | 0.5% | 2 of 16 | **<0.00005** | 0.90 | 0.90 |
| `BM_SingleThread_Baseline` | 1,401 us | 0.1% | 0.2% | 0.6% | 0.4% | 0 of 16 | **0.0001** | 0.20 | 0.06 |

**Ring buffer push and pop** (one thread)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_SPSC_SingleThread_Push` | 1.105 ns | 0.1% | 0.4% | 1.6% | 0.3% | 6 of 16 | **<0.00005** | 0.36 | 0.29 |
| `BM_SPSC_SingleThread_PushPop` | 1.566 ns | 0.3% | 0.2% | 0.6% | 0.7% | 0 of 16 | 0.0036 | 0.71 | 0.54 |

**SPSC ring buffer, producer to consumer** (two threads)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_SPSC_ProducerConsumer/10000/real_time` | 489.0 us | 2.9% | 17.1% | 169.9% | 8.4% | 9 of 16 | **<0.00005** | 0.51 | 0.64 |
| `BM_SPSC_ProducerConsumer/100000/real_time` | 6,120 us | 2.3% | 18.4% | 157.4% | 6.1% | 9 of 16 | **<0.00005** | 0.54 | 0.70 |
| `BM_SPSC_Order_ProducerConsumer/10000/real_time` | 536.2 us | 1.3% | 10.7% | 33.0% | 3.4% | 11 of 16 | **<0.00005** | 1.00 | 0.76 |

**Mutex queue, producer to consumer** (two threads)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_Mutex_ProducerConsumer/10000/real_time` | 2,911 us | 1.4% | 16.2% | 63.9% | 4.1% | 11 of 16 | **<0.00005** | 0.0080 | 0.14 |
| `BM_Mutex_ProducerConsumer/100000/real_time` | 29.13 ms | 1.3% | 20.3% | 82.0% | 3.1% | 14 of 16 | **<0.00005** | 0.07 | 0.34 |

**SPSC pipeline** (two threads)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_Pipeline_Throughput/10000/iterations:3` | 2,535 us | 16.3% | 6.0% | 20.9% | 48.8% | 0 of 16 | 1.00 | 0.61 | 0.66 |
| `BM_Pipeline_Throughput/50000/iterations:3` | 13.21 ms | 2.5% | 3.0% | 8.3% | 6.9% | 0 of 16 | **0.0002** | 0.04 | 0.25 |
| `BM_Pipeline_Throughput/100000/iterations:3` | 28.63 ms | 1.9% | 3.5% | 8.9% | 5.2% | 0 of 16 | **<0.00005** | 0.12 | 0.68 |

**Mutex pipeline** (two threads)

| Benchmark | Median | CV inside a launch | Middle half of launch medians | Spread of launch medians | Expected from scatter | Launches beyond scatter | p (launches) | p (sessions) | p (trend) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `BM_MutexPipeline_Throughput/10000/iterations:3` | 5,075 us | 5.2% | 33.0% | 41.4% | 13.4% | 7 of 16 | **<0.00005** | 0.96 | 0.31 |
| `BM_MutexPipeline_Throughput/50000/iterations:3` | 25.44 ms | 3.2% | 33.3% | 46.8% | 7.8% | 7 of 16 | **<0.00005** | 0.94 | 0.30 |
| `BM_MutexPipeline_Throughput/100000/iterations:3` | 50.83 ms | 1.8% | 35.1% | 42.8% | 5.1% | 11 of 16 | **<0.00005** | 0.96 | 0.25 |

## Launch medians round by round

One row per session, one column per round, in recording order. The letter after each value is the condition of that round: s for shared, c for cleared.

`BM_SPSC_ProducerConsumer/100000/real_time`, us

| Session | Round 1 | Round 2 | Round 3 | Round 4 | Round 5 | Round 6 | Round 7 | Round 8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4,974 s | 6,106 c | 5,880 s | 3,859 c | 6,831 s | 5,321 c | 6,395 s | 6,267 c |
| 2 | 5,122 c | 4,749 s | 3,864 c | 4,733 s | 6,376 c | 6,313 s | 6,640 c | 4,197 s |
| 3 | 6,237 s | 9,465 c | 5,084 s | 8,138 c | 5,840 s | 6,223 c | 6,683 s | 5,186 c |
| 4 | 6,134 c | 6,588 s | 3,677 c | 4,321 s | 6,243 c | 5,477 s | 5,785 c | 6,219 s |

`BM_Mutex_ProducerConsumer/100000/real_time`, ms

| Session | Round 1 | Round 2 | Round 3 | Round 4 | Round 5 | Round 6 | Round 7 | Round 8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 32.86 s | 26.24 c | 28.66 s | 31.97 c | 36.51 s | 26.79 c | 31.63 s | 32.29 c |
| 2 | 31.42 c | 27.83 s | 42.90 c | 33.19 s | 35.24 c | 26.63 s | 35.27 c | 28.59 s |
| 3 | 27.02 s | 33.87 c | 35.99 s | 27.33 c | 27.84 s | 23.58 c | 25.90 s | 26.77 c |
| 4 | 28.96 c | 26.84 s | 24.90 c | 28.32 s | 27.54 c | 38.05 s | 29.30 c | 28.40 s |

`BM_Pipeline_Throughput/100000/iterations:3`, ms

| Session | Round 1 | Round 2 | Round 3 | Round 4 | Round 5 | Round 6 | Round 7 | Round 8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 28.88 s | 27.44 c | 28.64 s | 28.02 c | 28.59 s | 29.89 c | 29.84 s | 29.14 c |
| 2 | 28.75 c | 29.50 s | 28.82 c | 29.15 s | 28.05 c | 28.39 s | 29.16 c | 28.61 s |
| 3 | 27.44 s | 27.75 c | 29.53 s | 28.04 c | 38.41 s | 27.87 c | 29.15 s | 27.98 c |
| 4 | 28.55 c | 28.32 s | 28.71 c | 29.05 s | 29.79 c | 27.37 s | 28.97 c | 27.42 s |

`BM_MutexPipeline_Throughput/100000/iterations:3`, ms

| Session | Round 1 | Round 2 | Round 3 | Round 4 | Round 5 | Round 6 | Round 7 | Round 8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 48.73 s | 68.21 c | 63.11 s | 50.52 c | 50.13 s | 48.53 c | 47.83 s | 51.13 c |
| 2 | 64.98 c | 49.67 s | 68.14 c | 66.90 s | 65.17 c | 67.82 s | 47.95 c | 47.47 s |
| 3 | 47.26 s | 66.16 c | 67.81 s | 66.76 c | 62.00 s | 49.36 c | 47.12 s | 47.97 c |
| 4 | 49.79 c | 50.25 s | 68.46 c | 47.14 s | 47.99 c | 67.94 s | 48.19 c | 47.19 s |

## shared against cleared

Median of the launch medians under each condition, the change from shared to cleared, and p for the two conditions differing, from shuffled condition labels.

| Benchmark | Median, shared | Median, cleared | Change | p | Spread, shared | Spread, cleared |
|---|---:|---:|---:|---:|---:|---:|
| `BM_AddOrder_NoMatch` | 37.77 ns | 37.82 ns | +0.1% | 0.99 | 6.5% | 4.9% |
| `BM_AddOrder_FullMatch` | 403.3 ns | 403.0 ns | -0.1% | 0.72 | 3.1% | 1.0% |
| `BM_AddOrder_SweepLevels/1` | 450.0 ns | 449.7 ns | -0.1% | 0.43 | 1.9% | 2.0% |
| `BM_AddOrder_SweepLevels/5` | 647.8 ns | 647.6 ns | -0.0% | 0.90 | 1.4% | 3.2% |
| `BM_AddOrder_SweepLevels/10` | 884.6 ns | 883.2 ns | -0.1% | 0.27 | 4.9% | 3.4% |
| `BM_AddOrder_SweepLevels/20` | 1,621 ns | 1,615 ns | -0.3% | 0.67 | 3.2% | 4.7% |
| `BM_CancelOrder` | 44.44 us | 44.45 us | +0.0% | 0.67 | 1.2% | 2.2% |
| `BM_MixedWorkload/iterations:50` | 134.0 ms | 134.0 ms | -0.0% | 0.54 | 1.7% | 2.0% |
| `BM_SingleThread_Baseline` | 1,397 us | 1,401 us | +0.3% | 0.01 | 2.3% | 0.6% |
| `BM_SPSC_SingleThread_Push` | 1.104 ns | 1.105 ns | +0.1% | 0.81 | 9.8% | 1.6% |
| `BM_SPSC_SingleThread_PushPop` | 1.566 ns | 1.566 ns | -0.0% | 0.54 | 0.9% | 0.6% |
| `BM_SPSC_ProducerConsumer/10000/real_time` | 472.6 us | 489.0 us | +3.5% | 0.98 | 92.2% | 169.9% |
| `BM_SPSC_ProducerConsumer/100000/real_time` | 5,860 us | 6,120 us | +4.4% | 0.87 | 62.8% | 157.4% |
| `BM_SPSC_Order_ProducerConsumer/10000/real_time` | 525.8 us | 536.2 us | +2.0% | 0.90 | 47.7% | 33.0% |
| `BM_Mutex_ProducerConsumer/10000/real_time` | 3,030 us | 2,911 us | -3.9% | 0.38 | 40.6% | 63.9% |
| `BM_Mutex_ProducerConsumer/100000/real_time` | 28.49 ms | 29.13 ms | +2.2% | 0.78 | 46.9% | 82.0% |
| `BM_Pipeline_Throughput/10000/iterations:3` | 2,575 us | 2,535 us | -1.6% | 0.42 | 37.4% | 20.9% |
| `BM_Pipeline_Throughput/50000/iterations:3` | 13.54 ms | 13.21 ms | -2.5% | 0.01 | 10.0% | 8.3% |
| `BM_Pipeline_Throughput/100000/iterations:3` | 28.76 ms | 28.63 ms | -0.5% | 0.51 | 40.3% | 8.9% |
| `BM_MutexPipeline_Throughput/10000/iterations:3` | 5,061 us | 5,075 us | +0.3% | 0.40 | 51.7% | 41.4% |
| `BM_MutexPipeline_Throughput/50000/iterations:3` | 25.18 ms | 25.44 ms | +1.1% | 0.54 | 52.1% | 46.8% |
| `BM_MutexPipeline_Throughput/100000/iterations:3` | 49.90 ms | 50.83 ms | +1.9% | 0.22 | 44.2% | 42.8% |

## What accompanies the movement

The rank correlation (Spearman ρ) of a benchmark's launch medians with what was recorded around each launch, for the benchmarks that move between launches. Only |ρ| ≥ 0.7 is listed. `benchmark:` values cover the benchmark's own part of the launch; the others cover the whole launch of its binary. 592 pairs of benchmark and recorded series were tested, and about 2 of them would reach that correlation without any relation. 21 of the 37 moving benchmarks have no such correlate and are not listed. A correlation does not say which of the two is the cause.

| Condition | Benchmark | Spread of launch medians | Correlates |
|---|---|---:|---:|
| shared | `BM_SPSC_ProducerConsumer/10000/real_time` | 92.2% | softirq_RCU per s (-0.71) |
| shared | `BM_Mutex_ProducerConsumer/10000/real_time` | 40.6% | foreign_ms per s (+0.72) |
| shared | `BM_SPSC_Order_ProducerConsumer/10000/real_time` | 47.7% | benchmark: CPU time per wall time (+0.81); benchmark: run-queue wait ms per s (-0.70) |
| shared | `BM_MutexPipeline_Throughput/10000/iterations:3` | 51.7% | softirq_TIMER per s (-0.79) |
| shared | `BM_MutexPipeline_Throughput/50000/iterations:3` | 52.1% | softirq_TIMER per s (-0.82) |
| shared | `BM_MutexPipeline_Throughput/100000/iterations:3` | 44.2% | softirq_RCU per s (-0.74); cpu_per_wall (+0.73); softirq_TIMER per s (-0.72) |
| cleared | `BM_MixedWorkload/iterations:50` | 2.0% | benchmark: run-queue wait ms per s (+0.75) |
| cleared | `BM_SPSC_ProducerConsumer/10000/real_time` | 169.9% | softirq_RCU per s (-0.83); benchmark: CPU time per wall time (+0.81); foreign_ms per s (-0.76); involuntary_switches per s (-0.73) |
| cleared | `BM_SPSC_ProducerConsumer/100000/real_time` | 157.4% | share of CPU time on cpu1 (-0.78); softirq_RCU per s (-0.75); foreign_ms per s (-0.72) |
| cleared | `BM_Mutex_ProducerConsumer/100000/real_time` | 82.0% | benchmark: run-queue wait ms per s (-0.71) |
| cleared | `BM_SPSC_Order_ProducerConsumer/10000/real_time` | 33.0% | benchmark: CPU time per wall time (+0.73) |
| cleared | `BM_Pipeline_Throughput/50000/iterations:3` | 8.3% | cpu_per_wall (+0.90); benchmark: CPU time per wall time (+0.76) |
| cleared | `BM_Pipeline_Throughput/100000/iterations:3` | 8.9% | benchmark: CPU time per wall time (+0.87); cpu_per_wall (+0.81); irq_LOC per s (+0.71) |
| cleared | `BM_MutexPipeline_Throughput/10000/iterations:3` | 41.4% | cpu_per_wall (+0.88); softirq_TIMER per s (-0.77) |
| cleared | `BM_MutexPipeline_Throughput/50000/iterations:3` | 46.8% | cpu_per_wall (+0.93); benchmark: CPU time per wall time (+0.92); softirq_TIMER per s (-0.74) |
| cleared | `BM_MutexPipeline_Throughput/100000/iterations:3` | 42.8% | benchmark: CPU time per wall time (+0.93); cpu_per_wall (+0.93); softirq_TIMER per s (-0.78) |

## How the two-thread benchmarks were scheduled

CPU time per wall time is 2 when both threads run all the time on separate cores and 1 when they share one core or one of them always waits. Run-queue wait is time a runnable task spent waiting for one of the two cores.

| Condition | Benchmark | CPU per wall, min | median | max | Run-queue wait ms per s, median | max |
|---|---|---:|---:|---:|---:|---:|
| shared | `BM_SPSC_ProducerConsumer/10000/real_time` | 1.90 | 1.94 | 1.95 | 7.8 | 11.3 |
| shared | `BM_SPSC_ProducerConsumer/100000/real_time` | 1.98 | 1.99 | 2.00 | 2.0 | 8.5 |
| shared | `BM_SPSC_Order_ProducerConsumer/10000/real_time` | 1.85 | 1.97 | 1.97 | 6.9 | 177.2 |
| shared | `BM_Mutex_ProducerConsumer/10000/real_time` | 1.95 | 1.97 | 1.98 | 3.0 | 6.8 |
| shared | `BM_Mutex_ProducerConsumer/100000/real_time` | 1.94 | 1.98 | 1.99 | 0.9 | 25.3 |
| shared | `BM_Pipeline_Throughput/10000/iterations:3` | 1.46 | 1.66 | 1.68 | 80.3 | 524.8 |
| shared | `BM_Pipeline_Throughput/50000/iterations:3` | 1.33 | 1.93 | 1.94 | 4.2 | 1347.7 |
| shared | `BM_Pipeline_Throughput/100000/iterations:3` | 1.49 | 1.96 | 1.96 | 2.2 | 1497.9 |
| shared | `BM_MutexPipeline_Throughput/10000/iterations:3` | 1.37 | 1.76 | 1.82 | 80.0 | 632.8 |
| shared | `BM_MutexPipeline_Throughput/50000/iterations:3` | 1.12 | 1.79 | 1.85 | 7.3 | 1163.0 |
| shared | `BM_MutexPipeline_Throughput/100000/iterations:3` | 1.46 | 1.78 | 1.85 | 6.4 | 674.5 |
| cleared | `BM_SPSC_ProducerConsumer/10000/real_time` | 1.90 | 1.94 | 1.97 | 6.8 | 13.9 |
| cleared | `BM_SPSC_ProducerConsumer/100000/real_time` | 1.97 | 1.99 | 2.00 | 1.7 | 7.4 |
| cleared | `BM_SPSC_Order_ProducerConsumer/10000/real_time` | 1.96 | 1.97 | 1.97 | 6.6 | 10.4 |
| cleared | `BM_Mutex_ProducerConsumer/10000/real_time` | 1.94 | 1.97 | 1.98 | 2.0 | 5.9 |
| cleared | `BM_Mutex_ProducerConsumer/100000/real_time` | 1.92 | 1.98 | 1.99 | 0.4 | 6.6 |
| cleared | `BM_Pipeline_Throughput/10000/iterations:3` | 1.51 | 1.66 | 1.67 | 57.8 | 372.3 |
| cleared | `BM_Pipeline_Throughput/50000/iterations:3` | 1.93 | 1.93 | 1.94 | 6.6 | 22.9 |
| cleared | `BM_Pipeline_Throughput/100000/iterations:3` | 1.95 | 1.96 | 1.96 | 1.2 | 5.9 |
| cleared | `BM_MutexPipeline_Throughput/10000/iterations:3` | 1.72 | 1.77 | 1.82 | 73.5 | 171.1 |
| cleared | `BM_MutexPipeline_Throughput/50000/iterations:3` | 1.78 | 1.79 | 1.85 | 7.4 | 21.5 |
| cleared | `BM_MutexPipeline_Throughput/100000/iterations:3` | 1.76 | 1.79 | 1.85 | 4.9 | 23.5 |

## Pipeline throughput ratio at 100,000 orders

SPSC pipeline throughput divided by mutex pipeline throughput, one value per round from the two launches of that round.

| Condition | Rounds | Lowest | Median | Highest |
|---|---:|---:|---:|---:|
| shared | 16 | 1.60× | 1.72× | 2.48× |
| cleared | 16 | 1.61× | 1.79× | 2.49× |

## Pipeline latency at 100,000 orders

In TSC ticks, the unit of the processor's timestamp counter; not converted to time because the counter's frequency has not been calibrated. Each launch prints one report per repetition; a launch is represented by the median of its reports.

| Condition | Pipeline | Percentile | Median of launches | Lowest launch | Highest launch | Spread of launch medians | Highest / lowest report inside a launch, median |
|---|---|---|---:|---:|---:|---:|---:|
| shared | Mutex pipeline | P50 | 1,060,414 | 198,263 | 3,160,860 | 1494.3% | 7.1× |
| shared | Mutex pipeline | P99 | 5,055,668 | 4,143,847 | 7,394,926 | 78.5% | 2.0× |
| shared | SPSC pipeline | P50 | 2,722,747 | 2,417,471 | 2,943,634 | 21.8% | 1.2× |
| shared | SPSC pipeline | P99 | 3,348,862 | 2,889,027 | 3,470,975 | 20.1% | 1.2× |
| cleared | Mutex pipeline | P50 | 777,810 | 254,365 | 2,905,627 | 1042.3% | 4.5× |
| cleared | Mutex pipeline | P99 | 4,557,820 | 3,654,095 | 7,861,276 | 115.1% | 1.6× |
| cleared | SPSC pipeline | P50 | 2,827,616 | 2,591,745 | 3,160,412 | 21.9% | 1.2× |
| cleared | SPSC pipeline | P99 | 3,351,426 | 2,962,888 | 3,476,273 | 17.3% | 1.2× |


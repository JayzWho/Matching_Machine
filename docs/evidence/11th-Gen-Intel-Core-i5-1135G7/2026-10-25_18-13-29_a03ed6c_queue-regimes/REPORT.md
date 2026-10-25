# What sets the queue benchmarks' level in each memory region

Recorded on 2026-10-25 on the 11th Gen Intel Core i5-1135G7, on branch
`bench/queue-regimes`. The [variance-sources study](https://github.com/JayzWho/Matching_Machine/blob/archive/variance-sources-2026-10/docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-19_11-49-57_e6a3630_variance-sources/REPORT.md)
found that each producer-to-consumer benchmark of `bench_spsc_ring_buffer` runs
at a level set by the memory its queue lands in: the int64 ring at about 9 or
about 50 ns per item, the Order ring and the mutex queue at levels that vary
continuously. This study asks why, so that the fixes can decide whether the
int64 ring's two levels are reported as a distribution or removed.

A **region** is one of the memory regions the diagnostic program maps at
start-up; benchmarks with the same region index share its memory. The
**orientation** is the CPU the producer runs on; the consumer runs on the other
measured CPU. A **cell** is one region in one orientation. The consumer has
**caught up** when the queue holds less than 1% of the items as the producer
finishes and the consumer had to wait for the producer before at least 1% of
them. A **round trip** passes a value through one cache line from one CPU to the
other and back.

## How it was recorded

One recording with `scripts/record/bench_diagnose.py` in the `cleared` condition
(`systemd --user` allowed to stay), with the VirtualBox VM saved: 2 sessions × 8
blocks of two conditions (the int64 benchmarks; the Order and mutex benchmarks),
16 launches each, 5 repetitions. `bench_queue_regimes` is generated from the
unchanged `bench_spsc_ring_buffer.cpp` by `scripts/diagnostics/build_queue_regimes.py`;
every added or changed line ends in `// DIAG`, and the retry counters add no
instruction to a successful try. Each benchmark runs on 8 regions per launch.
The scheduler places the threads, so the orientation is read per repetition; 284
of 3,840 repetitions moved the producer between CPUs and belong to no cell. All
32 launches had 0 thermal throttle events, 0 s suspended and 0 device interrupts
on the measured CPUs 3 and 1. All times are wall-clock time. The tables come from
`scripts/diagnostics/analyze_queue_regimes.py`.

The hypotheses and their criteria were written down before recording. After a
discarded wiring check, and before this recording, the criterion for "caught up"
was changed from "waits before 30% of the items" to the definition above: a
caught-up consumer receives items in small batches, not one at a time.

## Findings

**1. The int64 ring's two levels are two operating states.** In 174 of 235 cells
the producer stays ahead: the queue still holds 35% of the items when it finishes,
the consumer never waits, and an item takes 9.1 ns. In 60 cells the consumer has
caught up: it waits before about one item in twelve and finds the queue empty 1.4
times per item, and an item takes 57 ns. Slow means caught up in 100% of cells,
and in 99% for the copy that sums the checksum in a local.

**2. A cell has one level; region and orientation choose it together.** No cell
mixed slow and fast repetitions (479 cells), and a cell's repetitions differ with
a median CV of 0.5–0.8%. Of 107 regions measured in both orientations, 49 were
fast in both and 58 slow in exactly one; none was slow in both. The two int64
benchmarks agree in all 225 cells they share. The movement between launches came
from the one draw of memory per launch and from the scheduler's choice of
orientation.

**3. A region can be slow only when the consumer's index is the slower line to
move.** A round trip takes 263 ns at the median (240–293 ns from the 5th to the
95th percentile), and the repetitions of one line differ with a CV of 5.6%. In
the regions fast in both orientations, the line of `head_` (line 0) is 27 ns
faster than the line of `tail_` (line 1) at the median; in the regions slow in one
orientation it is 29 ns slower. The sign agrees with the region's
outcome in 96% of regions, and 59 of the 61 slow cells have the slower `head_`
line. A round trip is the same in both directions, so it does not tell which
orientation is the slow one. The criterion stated before recording, one threshold
on one region's round trips against its median level, agreed in at most 79%; the
grouping by orientation was added after the recording.

**4. A ring that caches the other side's index is caught up everywhere.** It ran
at one level in all 240 cells (99% within 25% of the median), 42 ns per item: its
consumer reads `tail_` only when its copy says the queue is empty, becomes the
faster side and catches up. It takes 41 ns per item where the original ring is
fast and 49 ns where it is slow.

**5. The Order ring and the mutex queue follow how often the consumer waits.**
The Order ring never lets the producer stay ahead: in every cell its consumer
waits before 12–28% of the items, and the queue holds under 2% of them when the
producer finishes. The mutex queue's consumer waits in 164 of 238 cells. Over
cells, their time per item rises with the consumer's waits per item (Spearman
0.97 and 0.93). In one launch and orientation their regions differ
with a CV of 8% and 8–16%, and moving the producer to the other CPU changes one
region's time by a median of +10% for the Order ring and by −20% to +9%
(quartiles) for the mutex queue. The round trips of their hot lines do not predict
them (|Spearman| ≤ 0.28).

| Benchmark | What sets its time | Class | Remedy |
|---|---|---|---|
| int64 ring | Two states, 8–9 or 57–62 ns per item; region and orientation choose one | The two states: property of the ring under a symmetric full-speed load. Their movement between launches: defect in sampling | Pin the threads, measure both orientations on many fresh regions, report each state's level and the share of slow cells |
| Order ring, mutex queue | How often the caught-up consumer waits, set by region and orientation | Property; defect in sampling | Pin the threads, many fresh regions, report the median |
| Ring caching the other index | Caught up in every cell, 42 ns per item | Property of that design | Caching alone is no improvement under this load |

## Limits

Which property of a physical address sets the cost of moving a line, and why the
orientation decides which cell of a region is slow, is not resolved. Answering
them needs the cost of each direction measured on its own, or the physical
addresses, which only root can read. The orientations were observed rather than
set, so not every region was measured in both.

## Reproduce

```bash
python3 scripts/diagnostics/build_queue_regimes.py
python3 scripts/record/bench_diagnose.py --matrix scripts/diagnostics/queue_regimes_matrix.json \
  --output <directory> --sessions 2 --blocks 8 --repetitions 5 \
  --interference cleared --allow-unmovable systemd
python3 scripts/diagnostics/analyze_queue_regimes.py <directory>
```

The recording was made at `a03ed6c`; the analysis script was added with this
report.

# What selects the int64 ring's state

Recorded on 2026-10-26 on the 11th Gen Intel Core i5-1135G7, on branch
`bench/queue-regimes`. The [queue-regimes study](https://github.com/JayzWho/Matching_Machine/blob/archive/queue-regimes-2026-10/docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-25_18-13-29_a03ed6c_queue-regimes/REPORT.md)
found that the int64 ring of `bench_spsc_ring_buffer` runs in one of two states,
the producer ahead at 8 to 9 ns per item or the consumer caught up at 57 to 62 ns,
and that the memory the ring lands in and the CPU of the producer choose the state
together. This study asks why the caught-up state costs seven times as much,
whether the start of an iteration can choose the state, and which property of
the memory selects it.

A **control page** is the first page of a ring. It holds `head_` on cache line 0,
`tail_` on line 1 and the first 496 slots. The diagnostic program builds each
ring from one control page and 128 **data pages** of one memory file, so rings
can share a control page and differ in their data pages. An **orientation** is the
CPU the producer is pinned to, 1 or 3; the consumer runs on the other one. A
**cell** is one ring in one orientation, and it is slow above 20 ns per item. A
**slice** is one of the four parts of the last-level cache; the physical address
of a line decides which slice serves it.

## How it was recorded

Two recordings with `scripts/record/bench_diagnose.py` in the `cleared` condition
(`systemd --user` allowed to stay), with the VirtualBox VM saved: 2 sessions ×
8 launches each, 3 repetitions of 10 iterations, all variants in random order
inside a launch. `variants/` holds the variants on 8 control pages per launch, and
the base variant and one-way timing on 64. `far-ed8444a/` adds starts with the
producer 40,000 items ahead, timestamps on both sides and round trips. The 32
launches had 0 thermal throttle events, 0 s suspended and 0 device interrupts on
the measured CPUs. `root-ed8444a/` is one launch as root in the same measurement
mode, with the user's other tasks moved off the two CPUs; it read the physical
pages and, from `uncore_cbox_0`–`3` lookup counts, the slice of each control line,
with 0 throttle events. `bench_queue_mechanism` is generated from the unchanged
`bench_spsc_ring_buffer.cpp` and `spsc_ring_buffer.h` by
`scripts/diagnostics/build_queue_mechanism.py`; every added or changed line ends
in `// DIAG`, and the script lists the hypotheses and criteria written before
each recording. The tables come from `scripts/diagnostics/analyze_queue_mechanism.py`.

## Findings

**1. The caught-up state is slow because the consumer reads the line the producer
is still writing.** Eight int64 slots share a line. In the 71 cells that were slow
under the control variant (52.1 ns per item), a consumer that takes a slot only
after the producer has left the slot's line ran at 24.3 ns: it removed 64% of the
gap to the fast level and still waited before 7% of the items. Staying four lines
further behind removed 91%, 64 lines 92%; fast cells kept their level. In slow
cells, 36% of the items sampled were read from a line the producer was still
writing; in fast cells none. The consumer's loads served by the producer's cache
came from data lines 0.34 times per item, on slots 0 to 6 of a line and never on
slot 7, after which the producer does not write the line again; in fast cells
0.008 times. With every item padded to a line of its own, all cells were slow.

**2. Each cell has one stable state: the side that is faster while the producer is
far ahead wins.** No start changed the state of any of 256 cells: the consumer
spinning first, the original start, or the producer 16,384 items ahead. The
original start gives the producer a lead of about 2,000 items. Slow cells are
caught up after 4,000 to 6,000 items; in fast cells the producer pulls away within
4,000 to 11,000 items even when the consumer starts first. Timestamps of both
sides, in TSC ticks per item, median over cells (`far-ed8444a/`, original start):

| Cells | Producer alone | Producer, queue ≥ 2048 | Consumer, queue ≥ 2048 | Both, queue < 512 |
|---|---|---|---|---|
| Fast, 194 | 5.0 | 18.4 | 28.1 | — |
| Slow, 62 | 5.0 | 26.7 | 15.5 | 165 |

The placement moves both sides by about 45%, in opposite directions. Once caught
up, an item takes both sides 165 ticks, six times what the consumer of a fast
cell needs while the producer is still pushing.

**3. The slice of `tail_` and the CPU of the producer select the state.** Changing
only the data pages under a control page, or mapping the same pages at a second
address, kept the state of all 256 cells, and anonymous memory as used in the
queue-regimes study had a similar share of slow cells (24% against 26%). Of 1,024
control pages measured in both orientations, 496 were fast in both, 254 slow only
with the producer on CPU 1, 274 only with it on CPU 3, and none slow in both. Each
bit of the slice number is an XOR of physical-address bits (352 of 352 lines), and
lines 0 and 1 of a page differ only in bit 6, which enters one of the two bits. So
`head_` and `tail_` always fall in the slice pair (0, 1), (1, 0), (2, 3) or (3, 2).
The state of all 128 cells of the root run followed from this: slow exactly when
`tail_` was in slice 1 with the producer on CPU 1 (16 pages) or in slice 3 with the
producer on CPU 3 (19 pages). Evenly spread slices give a quarter of the cells
slow and no page slow in both orientations, as recorded. One-way handoffs and round
trips predict the state of at most 85% of cells: the handoff of `tail_` to the consumer is
faster in slow cells (305 to 319 against 334 to 348 ticks), and lines in slices 1
and 3 make a round trip in 234 to 246 ns against 259 to 283 ns in slices 0 and 2,
which is why the queue-regimes study found the slower `head_` line in slow regions.

**4. In the slow state the producer waits for line ownership, the consumer for
loads.** Counters per thread, slow against fast cells: the producer took 140
against 23 cycles per item, 135 of them with its store buffer full, for the same
11 instructions, and requested ownership of a line 1.09 against 0.23 times per
item. The consumer took 140 against 25 cycles, 59 of them waiting on a load that
missed the L1 cache, and 36 against 17 instructions, the extra ones failed pops.
Memory-ordering clears rose from 0.13 to 0.61 per item and cost 7 cycles of
recovery. Over the pushes after a 40,000-item lead, a slow cell's producer
requested ownership 0.59 against 0.19 times per item. Counting the failed pops
left the slow level as it was (58.7 ns, 57.5 without); a pause after each failed
pop lowered it to 39.0 ns and changed no state.

**5. The slow level depends on how an iteration starts.** A slow cell averages a
stretch with the producer ahead and the caught-up rest. Its level was 60.8 ns with
the consumer first, 57.6 ns with the original start and 42.1 ns with the producer
16,384 items ahead, while fast cells stayed between 7.5 and 10 ns. In a wiring check,
opening hardware counters in the consumer thread delayed it enough to bring slow
cells to 42 ns. Differences of this kind would explain why earlier diagnostic
programs found slow levels of 44, 54 and 57 to 62 ns.

**6. The Order ring follows the same placement.** An Order item fills a line, so the
Order ring is always caught up. Where the int64 ring is slow, on the same control
page and orientation, the Order ring took 59.2 against 49.9 ns per item and its
consumer waited before 25% against 14% of the items.

| Behaviour | Cause | Class | Remedy |
|---|---|---|---|
| int64 ring: two levels | Whether `tail_`'s slice is the producer's sets which side is faster while apart; caught up, neighbouring slots share a line | Property of the ring under a symmetric full-speed load on this CPU | Pin the threads, measure both orientations on many fresh regions, report each state's level and the share of slow cells, one in four per orientation |
| The state a launch shows | One draw of memory per launch, and the scheduler's choice of CPUs | Defect in sampling | As above |
| Slow level between 39 and 61 ns | The start of each iteration and the consumer's spin loop | Property of the benchmark's design | Compare slow levels only within one program |
| Order ring ±19% | The same placement | Property; defect in sampling | Pinned threads, median over fresh regions |

## Limits

`head_` and `tail_` always sit in a slice pair, so this layout cannot separate the
slice of `tail_` from that of `head_`; that `tail_` is the active one rests on its
faster handoff. That slice n sits next to core n on the ring is inferred, not
measured. Why staying one line behind removes only 64% of the gap, for example
hardware prefetch reaching the lines the producer writes next, was not tested,
because switching the prefetchers off is a machine-wide setting. The planned perf
c2c recording was replaced by in-process sampling of the same cross-core load
event. One launch of `variants/` overlapped a compile on the housekeeping cores; its
slow level was 2 to 3 ns lower and no state changed. The mutex queue was not
measured.

## Reproduce

```bash
python3 scripts/diagnostics/build_queue_mechanism.py
python3 scripts/record/bench_diagnose.py --matrix scripts/diagnostics/queue_mechanism_matrix.json \
  --output <directory> --sessions 2 --blocks 8 --repetitions 3 \
  --interference cleared --allow-unmovable systemd
# The same with queue_mechanism_far_matrix.json for far-ed8444a/. For root-ed8444a/:
python3 scripts/diagnostics/root_session.py enter <directory>   # prints the command to run as root
python3 scripts/diagnostics/root_session.py leave <directory>
python3 scripts/diagnostics/analyze_queue_mechanism.py variants/
python3 scripts/diagnostics/analyze_queue_mechanism.py far-ed8444a/ root-ed8444a/
```

`variants/` was recorded at `bad47d0`; `far-ed8444a/` and `root-ed8444a/` at `ed8444a`.
The analysis script was added with this report.

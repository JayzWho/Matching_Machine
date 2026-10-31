# Benchmark evidence

The raw records behind the performance statements in this repository. How they
are produced is described in [`../BENCHMARKING.md`](../BENCHMARKING.md).

## What is here

| Directory | Contents | Status |
|---|---|---|
| [`11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/`](11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/) | The fixed benchmarks on the current bare-metal machine: 32 launches of each binary in 4 sessions. [`../RESULTS.md`](../RESULTS.md) reports it | Current control |

The measurements published from a cloud VM before the audit are kept only at
the tag `v1.0-vm-era`; [`../AUDIT.md`](../AUDIT.md) explains why they were
withdrawn.

## Completed studies

The current control and the workflow build on these results. The records of
the first two are kept at the tag `archive/pre-cleanup-2026-10`, those of the
third at `archive/variance-sources-2026-10`, those of the fourth at
`archive/queue-regimes-2026-10` and those of the fifth at
`archive/queue-mechanism-2026-10`.

- **Repeatability, 2026-10-12.** 32 launches of each unchanged binary in four
  sessions. Nothing drifted between sessions or over time, but results moved
  from one launch to the next: a single-threaded launch was usually within 1%
  of the others, while two-threaded benchmarks moved by tens of percent. A
  comparison between two versions of the code therefore needs many launches of
  each.
- **Launch variance, 2026-10-14.** The mutex pipeline ran at one of two levels,
  about 48 ms or about 66 ms for 100,000 orders, depending on the engine
  object's address modulo 64, which address-space randomisation changes at
  every launch. Hardware counters at offset 0 showed twice as many loads
  served by a cache line forwarded from the other core as at offset 16.
  Declaring the object `alignas(64)` fixes it at offset 0, the slower level.
  The launch-to-launch variation of the producer-to-consumer benchmarks in
  `bench_spsc_ring_buffer`, over the SPSC ring buffer and over a mutex queue,
  remained unexplained.
- **Variance sources, 2026-10-19.** Six diagnostic recordings traced what still
  moved between launches in the 2026-10-15 control. The SPSC pipeline's first
  repetition pays a one-time cost that one warm-up call removes; a longer
  warm-up exhausts the engine's memory pool and hangs. Both latency reports
  measure queue wait: service time moves by 0.4% to 3% between launches. The
  SPSC reports with a high maximum started with both workers on one CPU. In
  `BM_SPSC_ProducerConsumer` the consumer adds every item to a checksum on the
  cache line of `N`, which the producer reads for every item; moving the
  checksum makes the benchmark about five times faster. Below that, each queue
  benchmark runs at a level set by the memory its queue lands in, which a
  launch draws once, and the mutex queue also by the offset of its unaligned
  state. [Report](https://github.com/JayzWho/Matching_Machine/blob/archive/variance-sources-2026-10/docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-19_11-49-57_e6a3630_variance-sources/REPORT.md)
- **Queue regimes, 2026-10-25.** One recording asked why a queue benchmark's
  level follows the memory its queue lands in. With its checksum on a line of
  its own, the int64 ring runs in one of two states: the producer stays ahead,
  at 8 to 9 ns per item, or the consumer catches up and waits for the producer
  before about one item in twelve, at about 60 ns. One memory region with one
  assignment of the two threads to the two CPUs always gives the same state,
  and the two choose it together: a region can be slow only when the cache line
  of `head_` takes longer to move between the CPUs than the line of `tail_`,
  and then with the producer on only one of the CPUs. Which property of a
  physical address sets that cost is not resolved; the next study resolves it.
  A ring whose sides cache the other side's index was caught up in every
  region, at about 42 ns per item. The Order ring and the mutex queue slow
  down with how often their consumer waits. [Report](https://github.com/JayzWho/Matching_Machine/blob/archive/queue-regimes-2026-10/docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-25_18-13-29_a03ed6c_queue-regimes/REPORT.md)
- **Queue mechanism, 2026-10-26.** Two recordings and one launch as root asked
  why the int64 ring's caught-up state is slow and what selects it. Caught up,
  the consumer reads the cache line the producer is still writing: keeping the
  consumer one line behind removes 64% of the gap to the fast level, four lines
  91%. Each region in each orientation has one stable state, which no start of
  an iteration changes: the side that is faster while the producer is far ahead
  wins. The slice of the last-level cache that serves `tail_` decides which
  side that is: the ring is slow exactly when `tail_` is in the slice of the
  producer's CPU. That holds for one region in four per orientation and, since
  `head_` and `tail_` always fall in a pair of slices, never in both
  orientations. The same placement makes the Order ring about 19% slower.
  [Report](https://github.com/JayzWho/Matching_Machine/blob/archive/queue-mechanism-2026-10/docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-26_13-58-27_bad47d0_queue-mechanism/REPORT.md)

## Layout

One directory per machine, named after its CPU model. Inside it, one directory
per study, named `<date>_[<time>_]<commit>_<label>`:

| File | Contents |
|---|---|
| `REPORT.md` | Findings on one page. `scripts/report/bench_report.py <study>` prints the full tables |
| `samples.csv` | One row per repetition |
| `launches.csv` | One row per launch: validity checks and interference telemetry |
| `intervals.csv` | One row per benchmark per launch: wall time, CPU time, run-queue wait |
| `latency.csv` | The percentiles printed by the two latency-report benchmarks, in TSC ticks |
| `env.json` | Design, binary checksums, and the environment before and after each session |
| `raw.tar.gz` | Every raw file: Google Benchmark JSON, console output, `setup` output |
| `matrix.json` | Diagnostic studies only: the conditions with their executable, filter, prefix and environment overrides |

The CSV files are extracted from the files in `raw.tar.gz` at recording time.
A study made of several diagnostic recordings keeps `REPORT.md` at the top and
one subdirectory with the files above per recording.

## Rules

- A study is written by `scripts/record/bench_record.py` or `scripts/record/bench_diagnose.py`
  from a commit that contains every script it uses; `env.json` records the
  commit and the hash of each script. Recorded files are not edited afterwards.
  `REPORT.md` and figures are added by hand or by the scripts named in them.
- Raw output is stored once, inside `raw.tar.gz`.
- A study that has been replaced or answered leaves the tree only after a tag
  keeps it addressable. The tag is listed below with the reason.

## Commit IDs

On 2026-10-30 the history was rewritten to remove a directory of study notes,
`learning_notes/`. Every other file of every commit is unchanged, but every
commit ID changed. Study directories and `env.json` keep the IDs they were
recorded with; this table gives the rewritten commit for each.

| Recorded | Rewritten | Recorded | Rewritten |
|---|---|---|---|
| `46cf493` | `999c956` | `40a57bc` | `3d2513d` |
| `51201a0` | `7123870` | `54a8cfe` | `2e9b44b` |
| `575ea6e` | `ecaa7e8` | `6111ae4` | `6e61ced` |
| `65a2223` | `231c3d2` | `8cf43f1` | `b3c8f17` |
| `988720f` | `d0b3d84` | `a03ed6c` | `590c5ed` |
| `bad47d0` | `6797336` | `bde5e69` | `c083229` |
| `bf5f9fe` | `d6d42b4` | `e6a3630` | `682d4e7` |
| `e8870ee` | `bbf2695` | `ed8444a` | `76287ab` |

## Archived

| Tag | Contents | Why it left the tree |
|---|---|---|
| `archive/pre-cleanup-2026-10` | The two completed studies above with their reports; the tools only the launch-variance study used (object-placement build, GDB script, plot, hardware-counter summary, condition matrices); and material from the cloud VM period: `scripts/run_perf.sh`, `docs/build_commands.md`, `docs/optimization_log.md` and `docs/snapshots/` | The studies are answered and their conclusions are listed above. The VM-period material describes withdrawn measurements or a workflow that is no longer used |
| `archive/variance-sources-2026-10` | The variance-sources study: six recordings and their report, the script that builds its diagnostic programs, and the condition matrices | The study is answered and its conclusions are listed above. The fixes it calls for are made in their own pull requests |
| `archive/queue-regimes-2026-10` | The queue-regimes study: one recording and its report, the script that builds its diagnostic program, the condition matrix and the script that prints its tables | The study is answered and its conclusions are listed above. The fixes it calls for are made in their own pull requests |
| `archive/queue-mechanism-2026-10` | The queue-mechanism study: two recordings and one launch as root with their report, the script that builds its diagnostic program, the two condition matrices, the helper that holds measurement mode for the root launch, and the script that prints its tables | The study is answered and its conclusions are listed above. The fixes it calls for are made in their own pull requests |
| `archive/pipeline-fixes-2026-10` | The validation recording of #17: the pipelines with a warm-up repetition, the calibrated TSC, threads pinned in the process and the memory pool refilled | It served the pull request. The current control gives the numbers of the fixed code |
| `archive/two-thread-semantics-2026-10` | The validation recording of #18: the queue benchmarks on 16 memory regions in both placements, and the pipelines' latency split into queue time and service time | As above |
| `archive/order-book-fix-2026-10` | The check of #19, which fixed two order book defects | As above |
| `archive/bench-semantics-2026-10` | The check of #20, which fixed what the single-threaded benchmarks time and added the v1 order book to them; run outside measurement mode | As above |
| `archive/control-2026-10-15` | The control of 2026-10-15, recorded on the unfixed benchmarks; the one raw result file that survives from the cloud VM (`docs/evidence/historical_vm/`); and `docs/project_overview.md`, a Chinese write-up of the cloud VM period | The control of 2026-10-30 replaced the first. The other two hold only cloud VM numbers, which the audit withdrew |
| `archive/control-runs-2026-10` | Three recordings of the unfixed benchmarks with one launch per binary each (2026-09-22 and twice on 2026-10-07), their comparison as `REPRODUCIBILITY.md`, and the scripts that produced them | Three single launches cannot tell movement between launches from movement between sessions or boots. Replaced by the repeatability study |
| `archive/launch-variance-first-recording` | The first recording of the launch-variance studies with its tool snapshots, summaries, plots and one preflight that stopped before measuring, and its report `docs/LAUNCH_VARIANCE.md` | Recorded with tools that were not yet committed, in the shared condition only, while a VirtualBox VM was running. Replaced by the launch-variance study |
| `archive/mutex-lost-wakeup` | A stress program that runs the mutex pipeline back to back until it hangs, and a recording of the pipeline's throughput before and after the hang was fixed | Both served the fix of the mutex baseline's hang, which is complete. The fix changed throughput by less than the variation between launches |
| `v1.0-vm-era` | The repository as it stood when the cloud VM numbers were published, including the scripts under `benchmarks/bench_scripts/` | Frozen before the methodology audit |

To read an archived file: `git show <tag>:<path>`, or `git worktree add ../archive <tag>`.

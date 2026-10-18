# Benchmark evidence

The raw records behind the performance statements in this repository. How they
are produced is described in [`../BENCHMARKING.md`](../BENCHMARKING.md).

## What is here

| Directory | Contents | Status |
|---|---|---|
| [`historical_vm/`](historical_vm/) | The one raw result file that survives from the decommissioned cloud VM | Historical. Cannot be reproduced |
| [`11th-Gen-Intel-Core-i5-1135G7/2026-10-15_16-04-12_40a57bc_control/`](11th-Gen-Intel-Core-i5-1135G7/2026-10-15_16-04-12_40a57bc_control/) | The unfixed benchmarks on the current bare-metal machine, with the mutex baseline's hang fixed and its engine object aligned to a cache line: 32 launches of each binary in 4 sessions | Current control |

Measurements from the cloud VM and from the bare-metal machine are never
compared as if a code change separated them.

## Completed studies

The current control and the workflow build on these results. Their records
are kept at the tag `archive/pre-cleanup-2026-10`.

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

## Layout

One directory per machine, named after its CPU model. Inside it, one directory
per study, named `<date>_[<time>_]<commit>_<label>`:

| File | Contents |
|---|---|
| `REPORT.md` | Findings on one page. `scripts/bench_report.py <study>` prints the full tables |
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

- A study is written by `scripts/bench_record.py` or `scripts/bench_diagnose.py`
  from a commit that contains every script it uses; `env.json` records the
  commit and the hash of each script. Recorded files are not edited afterwards.
  `REPORT.md` and figures are added by hand or by the scripts named in them.
- Raw output is stored once, inside `raw.tar.gz`.
- A study that has been replaced or answered leaves the tree only after a tag
  keeps it addressable. The tag is listed below with the reason.

## Archived

| Tag | Contents | Why it left the tree |
|---|---|---|
| `archive/pre-cleanup-2026-10` | The two completed studies above with their reports; the tools only the launch-variance study used (object-placement build, GDB script, plot, hardware-counter summary, condition matrices); and material from the cloud VM period: `scripts/run_perf.sh`, `docs/build_commands.md`, `docs/optimization_log.md`, `docs/snapshots/` and `learning_notes/` | The studies are answered and their conclusions are listed above. The VM-period material describes withdrawn measurements or a workflow that is no longer used |
| `archive/control-runs-2026-10` | Three recordings of the unfixed benchmarks with one launch per binary each (2026-09-22 and twice on 2026-10-07), their comparison as `REPRODUCIBILITY.md`, and the scripts that produced them | Three single launches cannot tell movement between launches from movement between sessions or boots. Replaced by the repeatability study |
| `archive/launch-variance-first-recording` | The first recording of the launch-variance studies with its tool snapshots, summaries, plots and one preflight that stopped before measuring, and its report `docs/LAUNCH_VARIANCE.md` | Recorded with tools that were not yet committed, in the shared condition only, while a VirtualBox VM was running. Replaced by the launch-variance study |
| `archive/mutex-lost-wakeup` | A stress program that runs the mutex pipeline back to back until it hangs, and a recording of the pipeline's throughput before and after the hang was fixed | Both served the fix of the mutex baseline's hang, which is complete. The fix changed throughput by less than the variation between launches |
| `v1.0-vm-era` | The repository as it stood when the cloud VM numbers were published, including the scripts under `benchmarks/bench_scripts/` | Frozen before the methodology audit |

To read an archived file: `git show <tag>:<path>`, or `git worktree add ../archive <tag>`.

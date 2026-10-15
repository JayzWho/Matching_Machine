# Benchmark evidence

The raw records behind the performance statements in this repository. How they
are produced is described in [`../BENCHMARKING.md`](../BENCHMARKING.md).

## What is here

| Directory | Contents | Status |
|---|---|---|
| [`historical_vm/`](historical_vm/) | The one raw result file that survives from the decommissioned cloud VM | Historical. Cannot be reproduced |
| [`11th-Gen-Intel-Core-i5-1135G7/2026-10-12_15-11-09_e8870ee_control-repeatability/`](11th-Gen-Intel-Core-i5-1135G7/2026-10-12_15-11-09_e8870ee_control-repeatability/) | The unfixed benchmarks on the current bare-metal machine: 32 launches of each binary in 4 sessions | Previous control. The mutex pipeline starts at either of two levels |
| [`11th-Gen-Intel-Core-i5-1135G7/2026-10-14_13-32-00_65a2223_launch-variance/`](11th-Gen-Intel-Core-i5-1135G7/2026-10-14_13-32-00_65a2223_launch-variance/) | Six diagnostic recordings of the two-threaded benchmarks: address randomisation, fixed object placement, worker placement, native addresses, hardware counters, and the queue benchmarks | The mutex pipeline's two levels are set by the engine object's position within a cache line. The queue benchmarks remain unexplained |
| [`11th-Gen-Intel-Core-i5-1135G7/2026-10-15_16-04-12_40a57bc_control/`](11th-Gen-Intel-Core-i5-1135G7/2026-10-15_16-04-12_40a57bc_control/) | The same design with the mutex baseline's hang fixed and its engine object aligned to a cache line; the other three binaries unchanged | Current control. The mutex pipeline runs at one level |

Measurements from the cloud VM and from the bare-metal machine are never
compared as if a code change separated them.

## Layout

One directory per machine, named after its CPU model. Inside it, one directory
per study, named `<date>_[<time>_]<commit>_<label>`:

| File | Contents |
|---|---|
| `REPORT.md` | Findings. Studies before 2026-10-15 append the tables printed by `scripts/bench_report.py`; later ones give the command that prints them |
| `samples.csv` | One row per repetition |
| `launches.csv` | One row per launch: validity checks and interference telemetry |
| `intervals.csv` | One row per benchmark per launch: wall time, CPU time, run-queue wait |
| `latency.csv` | The percentiles printed by the two latency-report benchmarks, in TSC ticks |
| `env.json` | Design, binary checksums, and the environment before and after each session |
| `raw.tar.gz` | Every raw file: Google Benchmark JSON, console output, `setup` output |
| `matrix.json` | Diagnostic conditions and their executable, filter, prefix and environment overrides |

The CSV files are extracted from the files in `raw.tar.gz` at recording time.
A study made of several diagnostic recordings keeps `REPORT.md` and its figure
at the top and one subdirectory with the files above per recording.

## Rules

- A study is written by `scripts/bench_record.py` or `scripts/bench_diagnose.py`
  from a commit that contains every script it uses; `env.json` records the
  commit and the hash of each script. Recorded files are not edited afterwards.
  `REPORT.md` and figures are added by hand or by the scripts named in them.
- Raw output is stored once, inside `raw.tar.gz`.
- A study that has been replaced leaves the tree only after a tag keeps it
  addressable. The tag is listed below with the reason.

## Archived

| Tag | Contents | Why it left the tree |
|---|---|---|
| `archive/control-runs-2026-10` | Three recordings of the unfixed benchmarks with one launch per binary each (2026-09-22 and twice on 2026-10-07), their comparison in `REPORT`-like form as `REPRODUCIBILITY.md`, and the scripts that produced them | Three single launches cannot tell movement between launches from movement between sessions or boots. Replaced by the repeated-launch study above |
| `archive/launch-variance-first-recording` | The first recording of the launch-variance studies with its tool snapshots, summaries, plots and one preflight that stopped before measuring, and its report `docs/LAUNCH_VARIANCE.md` | Recorded with tools that were not yet committed, in the shared condition only, while a VirtualBox VM was running. Replaced by the re-recording above |
| `archive/mutex-lost-wakeup` | A stress program that runs the mutex pipeline back to back until it hangs, and a recording of the pipeline's throughput before and after the hang was fixed | Both served the fix of the mutex baseline's hang, which is complete. The fix changed throughput by less than the variation between launches |
| `v1.0-vm-era` | The repository as it stood when the cloud VM numbers were published, including the scripts under `benchmarks/bench_scripts/` | Frozen before the methodology audit |

To read an archived file: `git show <tag>:<path>`, or `git worktree add ../archive <tag>`.

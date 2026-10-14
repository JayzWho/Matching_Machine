# Benchmark evidence

The raw records behind the performance statements in this repository. How they
are produced is described in [`../BENCHMARKING.md`](../BENCHMARKING.md).

## What is here

| Directory | Contents | Status |
|---|---|---|
| [`historical_vm/`](historical_vm/) | The one raw result file that survives from the decommissioned cloud VM | Historical. Cannot be reproduced |
| [`11th-Gen-Intel-Core-i5-1135G7/2026-10-12_15-11-09_e8870ee_control-repeatability/`](11th-Gen-Intel-Core-i5-1135G7/2026-10-12_15-11-09_e8870ee_control-repeatability/) | The unfixed benchmarks on the current bare-metal machine: 32 launches of each binary in 4 sessions | Current control |
| [`2026-10-14` launch-variance studies](../LAUNCH_VARIANCE.md#evidence) | 248 launches testing process-local ASLR, object placement, worker CPU direction and hardware counters; one aborted preflight retained | Mutex pipeline's two levels traced to object address modulo 64; other queue variance remains unresolved |

Measurements from the cloud VM and from the bare-metal machine are never
compared as if a code change separated them.

## Layout

One directory per machine, named after its CPU model. Inside it, one directory
per study, named `<date>_[<time>_]<commit>_<label>`:

| File | Contents |
|---|---|
| `REPORT.md` | Findings, followed by the tables printed by `scripts/bench_report.py` |
| `samples.csv` | One row per repetition |
| `launches.csv` | One row per launch: validity checks and interference telemetry |
| `intervals.csv` | One row per benchmark per launch: wall time, CPU time, run-queue wait |
| `latency.csv` | The percentiles printed by the two latency-report benchmarks, in TSC ticks |
| `env.json` | Design, binary checksums, and the environment before and after each session |
| `raw.tar.gz` | Every raw file: Google Benchmark JSON, console output, `setup` output |
| `matrix.json` | Diagnostic conditions and their executable, filter, prefix and environment overrides |
| `artifacts/` | Diagnostic source/build snapshots, derived tables, plots and provenance |

The CSV files are extracted from the files in `raw.tar.gz` at recording time.

## Rules

- A study is written by `scripts/bench_record.py` or `scripts/bench_diagnose.py`.
  Recorded CSV, environment, command and raw output files are not edited
  afterwards. Reports and derived artifacts are added separately; source
  snapshots are checked against the hashes captured during recording.
- Raw output is stored once, inside `raw.tar.gz`.
- A study that has been replaced leaves the tree only after a tag keeps it
  addressable. The tag is listed below with the reason.

## Archived

| Tag | Contents | Why it left the tree |
|---|---|---|
| `archive/control-runs-2026-10` | Three recordings of the unfixed benchmarks with one launch per binary each (2026-09-22 and twice on 2026-10-07), their comparison in `REPORT`-like form as `REPRODUCIBILITY.md`, and the scripts that produced them | Three single launches cannot tell movement between launches from movement between sessions or boots. Replaced by the repeated-launch study above |
| `v1.0-vm-era` | The repository as it stood when the cloud VM numbers were published, including the scripts under `benchmarks/bench_scripts/` | Frozen before the methodology audit |

To read an archived file: `git show <tag>:<path>`, or `git worktree add ../archive <tag>`.

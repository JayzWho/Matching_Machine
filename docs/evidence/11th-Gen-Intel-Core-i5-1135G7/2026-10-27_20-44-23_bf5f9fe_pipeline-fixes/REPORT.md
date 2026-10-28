# Pipeline fixes: validation recording of 2026-10-27

The recording checks four fixes on branch `fix/pipeline-conditions`: the first
repetition left out as a warm-up, the TSC calibrated against `CLOCK_MONOTONIC`,
the two pipeline threads pinned at creation, and the engine's memory pool
refilled at `start()` with cancelled orders returned.

## Design

`scripts/record/bench_diagnose.py --matrix scripts/diagnostics/pipeline_fixes_matrix.json --allow-unmovable systemd`:
`cleared` condition, 2 sessions × 8 blocks, so 16 launches per condition,
6 repetitions per launch with the first a warm-up. The binaries are built by
`scripts/diagnostics/build_pipeline_variants.sh`:

| Condition | Program and filter | Commit |
|---|---|---|
| spsc-before | `bench_matching_engine`, `BM_Pipeline_` | e4a48b8 (master before the fixes) |
| spsc-pinned | same | e88ec88 (threads pinned) |
| spsc-pinned-pool | same | bf5f9fe (threads pinned, pool fixed) |
| mutex-before | `bench_baseline_mutex`, `BM_MutexPipeline_` | e4a48b8 |
| mutex-pinned | same | e88ec88 (the binary is identical at bf5f9fe) |

Machine: i5-1135G7 at its base frequency with turbo off and the SMT siblings
offline, kernel 7.0.0-34-generic, GCC 15.2.0 with `-O2 -DNDEBUG -std=c++20`.
Launches ran on CPUs 3 and 1; pinned threads put the producer on CPU 1 and the
consumer on CPU 3. The VirtualBox VM was saved. All 80 launches had 0 thermal
throttle events, 0 s suspended and 0 device interrupts on the measured cores;
foreign CPU time was about 1 ms per s (median), at most 10 ms per s.

## Results

Milliseconds per run of the pipeline: the median of 16 launch medians, with
the CV of the launch medians in brackets.

| Orders | spsc-before | spsc-pinned | spsc-pinned-pool | mutex-before | mutex-pinned |
|---|---:|---:|---:|---:|---:|
| 10,000 | 2.314 (1.4%) | 2.342 (1.4%) | 2.404 (2.6%) | 6.131 (4.4%) | 6.207 (4.2%) |
| 50,000 | 12.898 (2.4%) | 13.263 (2.0%) | 13.289 (2.1%) | 31.156 (3.1%) | 30.970 (3.4%) |
| 100,000 | 27.571 (1.9%) | 28.229 (1.8%) | 28.244 (1.3%) | 62.157 (3.3%) | 61.560 (3.8%) |

1. **Warm-up.** The first repetition of the SPSC pipeline at 10,000 orders
   takes 1.42 to 1.52 times as long as the others (median by condition) and is
   the slowest repetition in 46 of 48 launches. Leaving it out lowers the CV
   inside a launch from 16–20% to 3–4%. In the other benchmarks it takes
   1.01 to 1.04 times as long.
2. **Pinning.** The SPSC pipeline is slower pinned than placed by the
   scheduler: by 1.2% at 10,000 orders (p 0.05), 2.8% at 50,000 (p 0.002) and
   2.4% at 100,000 (p 0.004). The mutex pipeline does not change (−1.0% to
   +1.3%, p above 0.3). The cause was not investigated.
3. **Pool.** The pool fix makes the SPSC pipeline 2.7% slower at 10,000 orders
   (p below 0.001) and leaves 50,000 and 100,000 unchanged (+0.2% and +0.1%,
   p 0.96): a fixed cost of about 60 µs per run. Rebuilding the pool's free
   list of 131,072 entries in `start()`, inside the timed region, is the only
   fixed cost the fix adds; its duration was not measured separately.
4. **Latency tail.** No report in any condition has a maximum above 4.0M TSC
   ticks, the mark of a run whose two workers started on one CPU; 0 of the 96
   SPSC reports before the fixes. In the 2026-10-15 control, 25 of its 27 such
   reports were the first or second report of a launch, which follow the
   4-second single-threaded baseline. This recording's filter left that
   benchmark out and did not reproduce the condition that pinning removes.
5. **TSC.** All four calibrations, before and after each session, read
   2.419182 GHz in every interval.

`p` is the share of 20,000 shufflings of the launch medians between the two
conditions that give at least the observed difference of medians.

## Reproduce

```bash
scripts/diagnostics/build_pipeline_variants.sh
scripts/diagnostics/analyze_pipeline_fixes.py docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-27_20-44-23_bf5f9fe_pipeline-fixes
```

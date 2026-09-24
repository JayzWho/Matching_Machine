# Anchor measurement — v1-asis

**Verdict: VALID**

## What this is

Benchmark output from the **unfixed** code, measured on this machine. It is a
control group, not a performance claim.

The environment that produced the figures in README section 4 (a Tencent Cloud
VM, 2 vCPU @ 2494 MHz) has been decommissioned. Those numbers cannot be
reproduced, so they cannot serve as a baseline for anything measured here.
Comparing a future fix directly against them would confound a code change with
a hardware change. This run exists so that later fixes can be compared against
the same code on the same machine, isolating the code as the only variable.

## What is wrong with these numbers

Every defect documented in `docs/AUDIT.md` is present:

- `BM_MixedWorkload` and `BM_AddOrder_FullMatch` stop doing meaningful work
  after their first pass, because order state is never reset between iterations.
- `BM_CancelOrder` and `BM_MixedWorkload` include `~OrderBook()` in the
  timed region.
- `PauseTiming`/`ResumeTiming` adds a pedestal of roughly 800 ns per
  iteration to `BM_AddOrder_FullMatch` and `BM_AddOrder_SweepLevels`.
- Pipeline end-to-end latency measures queueing delay, not service latency.
- `items_per_second` in the multithreaded JSON is normalised by CPU time and
  is meaningless; derive throughput from `real_time`.
- TSC values are reported with a hardcoded 2.494 GHz conversion from the
  decommissioned VM.

**Do not quote any figure in this directory as a performance result.**

## Provenance
- commit: `575ea6e`
- date: 2026-09-22_16-04-18
- repetitions: 10
- single-thread affinity: `taskset -c 3`
- two-thread affinity: `taskset -c 1,3`
- throttle delta: core +0, package +0
- core selection ranking (best first): cpu3[3,7] throttle=1 irqs=690850; cpu1[1,5] throttle=17 irqs=664103; cpu2[2,6] throttle=2815 irqs=31942654; cpu0[0,4] throttle=19050 irqs=2877013
- device IRQs delivered to measurement cores during the run: cpu3 +0, cpu1 +0
  (informational: bench_env.sh steers movable IRQs away; kernel-managed or
  per-cpu IRQs cannot be moved and may still fire here)
- environment: see `env_before.json` / `env_after.json`

# Anchor measurement — v1-asis-kernel-7.0.0-34-repeat

**Verdict: VALID**

**Same measurement cores as the anchor: yes — 3,1, as in anchor 2026-09-22_16-04-18_575ea6e_v1-asis**

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
- commit: `54a8cfe`
- date: 2026-10-07_12-53-00
- repetitions: 10
- single-thread affinity: `taskset -c 3`
- two-thread affinity: `taskset -c 1,3`
- throttle delta: core +0, package +0
- measurement cores: `3,1`, selected by: pinned (/etc/matching-machine-bench.conf)
- live core ranking at setup (advisory only; its inputs reset on reboot): cpu2[2,6] throttle=0 irqs=38209320; cpu0[0,4] throttle=3 irqs=3440656; cpu3[3,7] throttle=55 irqs=4873425; cpu1[1,5] throttle=93 irqs=804624
- device IRQs delivered to measurement cores during the run: cpu3 +0, cpu1 +0
  (informational: bench_env.sh steers movable IRQs away; kernel-managed or
  per-cpu IRQs cannot be moved and may still fire here)
- environment set up by: `/usr/local/sbin/mm-bench-env` (sha256 `b0481f0fa2e24f826c1d5003508f46cdd72a33464d25dc30f115207b0787e4bd`); matches committed `scripts/bench_env.sh`: yes
- environment: see `env_before.json` / `env_after.json`

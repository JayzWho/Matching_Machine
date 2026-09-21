# Benchmark Methodology Audit

**Status:** in progress · **Opened:** 2026-09-20 · **Audited revision:** `929e0db` (tag `v1.0-vm-era`)

---

## 1. Purpose

This document records a methodology audit of the performance claims published in
`README.md` section 4. It exists because those claims were produced on a cloud VM
that has since been decommissioned, and because a review of the benchmark sources
found defects that make several of the published numbers measure something other
than what they are labelled as.

The audit is published rather than quietly fixed, for two reasons. The numbers are
already public, so silently replacing them would be worse than annotating them. And
the reasoning that identifies a bad benchmark is the same reasoning the project is
meant to demonstrate in the first place.

Nothing in the historical record is deleted. Raw evidence recovered from git history
is archived under `docs/evidence/historical_vm/`.

---

## 2. Measurement environments

| | Historical (published numbers) | Current (re-benchmark target) |
|---|---|---|
| Machine | Tencent Cloud Ubuntu VM `VM-0-3-ubuntu` | HP laptop, Ubuntu |
| CPU | 2 vCPU @ 2494 MHz | Intel Core i5-1135G7, 4C/8T |
| Frequency | fixed (`cpu_scaling_enabled: false`) | 400 MHz – 4.2 GHz, turbo enabled, `powersave` governor |
| L3 | 37.4 MB (shared by 2 vCPU) | 8 MB (shared by 8 threads) |
| SMT topology | not observable | core0={cpu0,cpu4}, core1={cpu1,cpu5}, … |
| Hardware PMU | unavailable (virtualized) | available (`arch_perfmon`, `pebs`, `intel_pt`) |
| TSC | `constant_tsc`, assumed 2.494 GHz | `constant_tsc nonstop_tsc tsc_known_freq`, frequency requires calibration |
| Status | **decommissioned — not reproducible** | active |

The two environments differ in core count, cache hierarchy, L3 capacity and frequency
behaviour. No number from one is comparable to a number from the other, and no
difference between them may be described as a code-level speedup.

---

## 3. Status of published claims

| Claim (README §4) | Status |
|---|---|
| §4.1 `BM_AddOrder_NoMatch` 83.5 → 46.4 ns | **Questionable** — measurement method broadly sound, but includes an unconditional `malloc`/`free` pair, and the two columns were produced with different benchmark code |
| §4.1 `BM_CancelOrder` 60,456 → 26,659 ns | **Withdrawn** — the timed region includes `~OrderBook()` (C3) |
| §4.1 `BM_AddOrder_SweepLevels/20` 3,093 → 2,297 ns | **Magnitude unreliable** — ~800 ns fixed pedestal from `PauseTiming` (C4) |
| §4.1 `BM_MixedWorkload` 3.87 → 6.19 M ops/s (+60%) | **Withdrawn** — cross-iteration state contamination (C2) |
| §4.1 "latency stddev / CV" | **Mislabelled** — run-to-run batch variability, not per-order jitter |
| "hash map 32% → 12% (perf measured)" | **Evidence chain broken** — profiled the contaminated benchmark, with unreliable frame-pointer stacks |
| §4.2 throughput 1.19 → 3.15 M/s (2.65×) | **Stands** — correctly derived from `real_time`; requires re-measurement on the new machine |
| §4.3 P50/P95/P99/P999 latency table | **Values real, interpretation wrong** — measures queueing delay, not service latency (C1) |
| §4.3 "bare-metal SPSC P50 typically reaches 200–800 ns" | **Withdrawn** — not achievable under the current design (C1) |
| §4.4 noalloc +30% (6.19 → 8.04 M ops/s) | **Withdrawn** — compares two different workloads (C5) |
| §5 "33 GTest cases … FeedSimulator" | **Incorrect** — 44 cases; no FeedSimulator test target exists |
| §5 "abseil-cpp pulled automatically" | **Incorrect** — `find_package(absl REQUIRED)` |
| §3 "cross-thread latency safe under SPSC acquire/release" | **Conceptually wrong** — acquire/release governs visibility, not TSC comparability across cores |

---

## 4. Confirmed findings

### C1 — End-to-end latency measures queueing delay, not matching latency

The producer writes a TSC timestamp before `try_push`; the consumer takes the
difference after `add_order_noalloc` returns. That interval is *queue wait + service
time*. The producer (PRNG + timestamp + push) outruns the consumer (map + hash +
match), so the 4096-slot ring is saturated for essentially the whole run, and each
order waits behind a nearly full queue.

The published numbers confirm this arithmetic:

```
100K orders / 31.7 ms real time  =  317 ns per order (service time)
4095 queued orders × 317 ns      =  1.30 ms expected queue wait
measured P50                     =  1.26 ms          (3% agreement)
```

The mutex baseline behaves the same way with an unbounded `std::queue`: 840 ns/order
service time and a 4.77 ms P50 implies a median backlog of ~5,700 orders.

So the "3.8× P50 improvement" is principally the ratio of a bounded 4096-slot ring to
an unbounded queue, multiplied by the 2.65× service-rate difference. It is not a
latency property of the synchronisation primitive.

The consequence for the published extrapolation is decisive: even if bare metal cut
service time to 150 ns, P50 would be ≈ 4095 × 150 ns ≈ 0.6 ms. The claim that bare
metal "typically reaches 200–800 ns" is unreachable by three orders of magnitude, and
the attribution of millisecond latency to hypervisor preemption is an assumption
inherited from the VM environment that a bare-metal re-run would contradict.

**Remediation:** report `service_latency`, `queue_delay` and `e2e_latency` as three
separate quantities, and drive the pipeline with a rate-limited (open-loop) generator
so latency can be reported as a function of offered load rather than as a single
number taken at saturation.

### C2 — Cross-iteration state contamination in `BM_MixedWorkload`

`benchmarks/bench_order_book.cpp:267` generates 1,000,000 orders outside the timing
loop. `add_order` mutates `Order::filled_qty`. The loop body constructs a fresh
`OrderBook` but never resets the orders.

From the second iteration onward, previously filled orders report `is_filled() == true`,
so `match()`'s `while (!incoming->is_filled() …)` never executes and the order is never
inserted into a price level. What remains is a hash insert, a hash erase and one
`malloc`/`free` from `trades.reserve(4)`. With `->Iterations(50)`, the reported mean is
dominated by this degenerate steady state.

Two independent confirmations: `BM_SingleThread_Baseline`
(`benchmarks/bench_matching_engine.cpp:103-106`) *does* reset `filled_qty` and `status`,
so the hazard was known but not applied consistently; and the degenerate path is
precisely the workload that most flatters the `flat_hash_map` and mutex-removal
changes being measured.

This also invalidates the profiling evidence, because `scripts/run_perf.sh` samples
this benchmark.

### C3 — `BM_AddOrder_FullMatch` stops doing work after 10,000 iterations

`benchmarks/bench_order_book.cpp:124` wraps `idx` back to 0 without resetting order
state. The benchmark does not fix its iteration count, so the framework runs it far
past 10,000 iterations; beyond that point every order is already `FILLED` and the timed
region contains two hash operations and nothing else.

This explains the published result directly: "optimized is 7.9% slower, within noise"
is not noise. Both columns measured an empty loop plus the timing pedestal described
in C4.

### C4 — `PauseTiming`/`ResumeTiming` imposes an ~800 ns pedestal

`BM_AddOrder_FullMatch`, `BM_AddOrder_SweepLevels` and `BM_CancelOrder` call
`PauseTiming`/`ResumeTiming` once per iteration. Each pair stops and restarts both a
real-time and a thread-CPU timer.

The project's own table shows the floor this creates:

| Benchmark | Reported | Timed work |
|---|---|---|
| `BM_AddOrder_FullMatch` | 756.8 ns | one exact match |
| `BM_AddOrder_SweepLevels/1` | 872.6 ns | sweep 1 level |
| `BM_AddOrder_SweepLevels/5` | 1,229.4 ns | sweep 5 levels |
| `BM_AddOrder_NoMatch` *(no pause in timed path)* | **46.4 ns** | one insert |

Workloads differing several-fold in cost all land near 800 ns, while the only
benchmark that pauses once per 1,000 iterations reports 46 ns. The ~800 ns is the
instrument, not the code. It also dilutes the SweepLevels/20 improvement, whose real
work is closer to 2,297 − 800 ≈ 1,500 ns.

### C5 — `BM_CancelOrder` times object destruction

`benchmarks/bench_order_book.cpp:242` declares `OrderBook book` *inside* the loop body.
`ResumeTiming()` is followed by one `cancel_order`, then the loop body ends and the
destructor runs inside the timed region: a 65,536-slot `flat_hash_map` (~1 MB), 1,000
`std::map` nodes and 1,000 `std::vector` instances.

A cancel is a hash lookup and a pointer store — a ~100 ns operation. The reported
26,659 ns is off by more than two orders of magnitude, which is the signal that the
timed region is wrong. The "56% improvement" is the difference between tearing down
1,000 `std::deque` instances and 1,000 `std::vector` instances; it is largely unrelated
to the lazy-deletion change it is cited as evidence for.

`BM_MixedWorkload` has the same structural problem.

### C6 — Hardcoded TSC-to-nanosecond conversion, and incorrect terminology

`bench_matching_engine.cpp:143` and `bench_baseline_mutex.cpp:454` both call
`report(/*cpu_ghz=*/2.494)`. 2.494 GHz is the decommissioned VM's frequency.

`include/latency_recorder.h` names the parameter `cpu_ghz` and prints values labelled
`cycles`. RDTSC returns **TSC ticks**, which on a machine whose cores range from
400 MHz to 4.2 GHz are not core cycles and do not convert to nanoseconds via the core
frequency. `learning_notes/07_latency_measurement.md:258` already states this correctly;
the code does not implement it.

**Remediation:** calibrate the TSC against `CLOCK_MONOTONIC` at startup, record the
calibration and its residual in run metadata, report values as TSC ticks, and remove
all hardcoded frequency literals.

### C7 — Published numbers have no recoverable raw evidence

- `results/` is listed in `.gitignore`; the directory exists neither in the repository
  nor on disk.
- `learning_notes/10_optimization.md:370` cites
  `results/order_book_2026-04-14_*.json` as the source of every README §4.1 figure.
  Those files do not exist.
- `docs/optimization_log.md` still contains a results table filled with
  `_(填入实测数据)_` and a `TODO`.
- `docs/optimization_log.md` references git tags `v0.2-before-opt` and `v0.2-after-opt`.
  No tags existed in the repository prior to this audit.
- Commit `dfcc2d0` modified `benchmarks/bench_order_book.cpp`, `include/order_book.h`
  and `src/order_book.cpp` together, so the "baseline" and "optimized" columns were
  produced by different benchmark code.

One raw file was recoverable from git history (deleted in `1c35b37`) and is archived at
`docs/evidence/historical_vm/2026-04-08_bench_order_book_after_opt.json`. It does not
support the published table — it is an earlier and visibly broken generation of the
benchmark:

```
BM_AddOrder_NoMatch/iterations:100000        277.2 ns   (README publishes 46.4 ns)
BM_AddOrder_SweepLevels/1                 12,824.5 ns
BM_AddOrder_SweepLevels/20                11,825.1 ns   <- sweeping 20 levels is
BM_CancelOrder/iterations:50000          902,399.3 ns      FASTER than sweeping 1
```

Sweeping 20 price levels cannot be cheaper than sweeping 1. Every benchmark in this
file also carries an explicit `iterations:N` suffix that the current sources no longer
set. The file is retained as a record of what was actually run, not as a supporting
measurement.

Separately, the repository does not currently build: `CMakeLists.txt` requires
`find_package(absl REQUIRED)`, and the audit machine has only the Abseil runtime
package, not the development package.

---

## 5. Additional confirmed issues

Tracked with the same rigour but lower blast radius:

- **Memory pool is monotonically consumed.** `cancel_order` never returns the cancelled
  resting order to the pool, the consumer's `OrderBook` is function-local and discards
  every still-resting order on exit, and `MatchingEngine::start()` does not reset the
  pool. The producer's allocation loop has no exit condition, so exhaustion is a hang.
  Commit `550a1a5` addressed the symptom by enlarging the pool; `LargeBatch_NoHang` is a
  regression test for the symptom. Also makes the three throughput iterations
  non-comparable.
- **Multithreaded benchmarks do not call `UseRealTime()`.** Google Benchmark normalises
  rate counters by `cpu_time`, which excludes user-spawned threads, producing
  `items_per_second` up to 1.03 G/s in the archived JSON. The narrative text works around
  this by hand; the code does not. `bench_spsc_ring_buffer.cpp` sets it correctly.
- **The noalloc +30% comparison changes five variables at once**: working set (1M vs 10K
  orders), cancel ratio (0.1 vs 0.2), the C2 contamination, translation-unit boundary
  (out-of-line call vs header template), and a no-op deallocator that is removable dead
  code.
- **RDTSC is neither serialised nor fenced against the compiler**, and cross-core TSC
  comparability is asserted rather than verified.
- **`scripts/run_perf.sh` selects the call-graph mode on the wrong criterion** (it picks
  DWARF when permissions are insufficient, and frame pointers otherwise), while the
  Release build does not pass `-fno-omit-frame-pointer`.
- **Thread placement is left to an external `taskset -c 0,1`** with no in-process
  affinity, no SMT sibling awareness, and no `pause` instruction in any spin loop.
- **No repetitions, warm-up or variability reporting** for the pipeline benchmark that
  produces the headline figures, and the current machine runs with a `powersave`
  governor and turbo enabled.

---

## 6. Verified as sound

The audit found the following correct; they are not scheduled for rework:

- `SPSCRingBuffer` memory ordering (relaxed on own index, acquire on the peer's,
  release on publish), cache-line separation of `head_`/`tail_`, power-of-two masking,
  and the sacrificed slot distinguishing full from empty.
- `BM_AddOrder_NoMatch`'s batch-rebuild structure — fixed background depth, one pause
  per 1,000 iterations, pre-generated orders, iteration count left to the framework.
  This is the pattern the other microbenchmarks should adopt.
- `bench_baseline_mutex.cpp` being fully self-contained and structurally symmetric with
  the version it is compared against.
- The `items_per_second` / `cpu_time` trap was correctly identified in the project notes
  and worked around by deriving throughput from `real_time`.
- The distinction between TSC ticks and core cycles was correctly stated in the notes.
- `run_all_benchmarks.sh`'s archival conventions (repetitions, aggregates, timestamped
  and tagged output).
- Integer pricing, `Order` at exactly 64 bytes with a static assertion, heap-allocated
  pool storage, fixed PRNG seed, ASan/UBSan on the debug build, and the absence of
  `-march=native`, LTO or PGO.

---

## 7. Remediation plan

| Phase | Scope |
|---|---|
| 0 | Freeze the VM-era snapshot (`v1.0-vm-era`), publish this audit, archive recovered evidence |
| 1 | Make the repository build; capture full environment metadata on every run |
| 2 | **Anchor measurement**: run the unmodified, still-defective code on the new machine and archive it as the only legitimate control group for subsequent fixes |
| 3 | Measurement infrastructure: TSC calibration, serialised RDTSC, pre-faulted latency recorder, in-process affinity |
| 4 | Benchmark semantic fixes, one per pull request, each re-run against the phase-2 anchor |
| 5 | Implementation correctness: memory pool reclamation |
| 6 | Pipeline v2: separated service/queue/end-to-end latency, open-loop load generator, queue-depth sweep |
| 7 | Hardware PMU evidence, now available on bare metal |
| 8 | Rewrite README §4 with historical and current results side by side, never overwriting |

Phase 2 is load-bearing. Because the original environment is gone, the only way to
attribute a change to code rather than to hardware is to measure the unfixed code on
the new machine first.

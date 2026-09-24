# Analysis — anchor measurement `v1-asis`

> **This is a control group, not a performance result.** Every number below was
> produced by benchmark code with the defects documented in `docs/AUDIT.md`. It
> exists so that later fixes can be compared against the same code on the same
> machine. None of these numbers may be compared with the historical VM figures
> in README section 4 as evidence of a code-level change: the machines differ.

## Conditions

- commit `575ea6e` (engine and benchmark code identical to `145b1b6` on master)
- 11th Gen Intel(R) Core(TM) i5-1135G7 @ 2.40GHz, kernel 7.0.0-31-generic
- c++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0; flags `-O2 -DNDEBUG -std=c++20` (from `compile_commands.json`)
- frequency: governor `performance`, `no_turbo=1`, `min/max_perf_pct=100/100`
- online CPUs `0,1,2,3` (SMT siblings offline); measurement cores `3,1`
- device IRQs steered off measurement cores: 60 moved, 12 refused; still delivered there: `120:dmar0-prq@cpu1 122:dmar1@cpu3`
- 10 repetitions per benchmark; throttle delta across the run +0 core / +0 package; device IRQs on measurement cores during the run +0 / +0

## Frequency verification (before the run, same measurement-mode session)

Both measurement cores were loaded for 60 s simultaneously (`frequency_check/burn.c`),
matching the two-thread shape of the pipeline benchmarks.

| Core | Core clock (perf `cycles`) | Reference clock (`ref-cycles`) | cycles / ref-cycles | `scaling_cur_freq` range |
|---|---:|---:|---:|---:|
| cpu1 | 2.394 GHz | 2.419 GHz | 0.9898 | 2399–2400 MHz (55 samples) |
| cpu3 | 2.394 GHz | 2.419 GHz | 0.9898 | 2399–2400 MHz (55 samples) |

No throttling occurred during the check. `ref-cycles` counts at a constant reference
rate close to the TSC rate, so ~2.419 GHz is an *estimate* of this machine's TSC
frequency — neither the nominal 2.4 GHz nor the 2.494 GHz hardcoded in the
benchmarks. A calibrated TSC frequency is Phase 3 work; until then latency is
reported in TSC ticks only.

## Results

`bench_order_book.json` — `taskset -c 3`

| Benchmark | Median (real time) | CV | Known defect |
|---|---:|---:|---|
| `BM_AddOrder_NoMatch` | 39.5 ns | 0.36% | includes one malloc/free (`trades.reserve(4)`) |
| `BM_AddOrder_FullMatch` | 405.4 ns | 0.25% | C3: no work after 10,000 iterations; C4 pedestal |
| `BM_AddOrder_SweepLevels/1` | 449.7 ns | 0.06% | C4 pedestal |
| `BM_AddOrder_SweepLevels/5` | 648.2 ns | 0.23% | C4 pedestal |
| `BM_AddOrder_SweepLevels/10` | 884.3 ns | 1.89% | C4 pedestal |
| `BM_AddOrder_SweepLevels/20` | 1,660.9 ns | 4.53% | C4 pedestal |
| `BM_CancelOrder` | 44,996.1 ns | 0.64% | C5: times `~OrderBook()` |
| `BM_MixedWorkload/iterations:50` | 134,870,798.5 ns | 1.51% | C2: degenerate after iteration 1; C5 |

`bench_spsc_ring_buffer.json` — `taskset -c 1,3`

| Benchmark | Median (real time) | CV | Known defect |
|---|---:|---:|---|
| `BM_SPSC_SingleThread_Push` | 1.4 ns | 1.86% |  |
| `BM_SPSC_SingleThread_PushPop` | 1.6 ns | 0.23% |  |
| `BM_SPSC_ProducerConsumer/10000/real_time` | 379,195.7 ns | 7.05% | includes thread create/join |
| `BM_SPSC_ProducerConsumer/100000/real_time` | 4,774,601.5 ns | 4.77% | includes thread create/join |
| `BM_Mutex_ProducerConsumer/10000/real_time` | 3,508,658.0 ns | 11.22% | M4: consumer spins holding the lock |
| `BM_Mutex_ProducerConsumer/100000/real_time` | 33,720,442.6 ns | 3.27% | M4: consumer spins holding the lock |
| `BM_SPSC_Order_ProducerConsumer/10000/real_time` | 500,552.0 ns | 1.09% |  |

`bench_matching_engine.json` — `taskset -c 1,3`

| Benchmark | Median (real time) | CV | Known defect |
|---|---:|---:|---|
| `BM_Pipeline_Throughput/10000/iterations:3` | 2.5 ms | 12.78% | H1 pool leak across iterations; thread start/stop |
| `BM_Pipeline_Throughput/50000/iterations:3` | 13.5 ms | 2.03% | H1 pool leak across iterations |
| `BM_Pipeline_Throughput/100000/iterations:3` | 29.1 ms | 2.03% | H1 pool leak across iterations |
| `BM_SingleThread_Baseline` | 1,402.8 us | 0.27% | no-op deallocator (removable dead code) |
| `BM_Pipeline_LatencyReport/iterations:1` | 5.3 ms | 5.44% | timed region excludes the work (Pause/Resume) |

`bench_baseline_mutex.json` — `taskset -c 1,3`

| Benchmark | Median (real time) | CV | Known defect |
|---|---:|---:|---|
| `BM_MutexPipeline_Throughput/10000/iterations:3` | 5.0 ms | 5.84% | M4: harness `alive` vector growth |
| `BM_MutexPipeline_Throughput/50000/iterations:3` | 24.9 ms | 2.94% | M4: harness `alive` vector growth |
| `BM_MutexPipeline_Throughput/100000/iterations:3` | 49.4 ms | 2.14% | M4: harness `alive` vector growth |
| `BM_MutexPipeline_LatencyReport/iterations:1` | 5.3 ms | 7.97% | timed region excludes the work (Pause/Resume) |

`items_per_second` in the multithreaded JSON is normalised by the benchmark
thread's CPU time, which excludes the worker threads; it is meaningless here and
is not reported. Throughput below is derived from `real_time`.

### Pipeline throughput (from real time)

| Orders | SPSC pipeline | Mutex baseline | SPSC / mutex |
|---:|---:|---:|---:|
| 10,000 | 3.95 M/s | 1.99 M/s | 1.98× |
| 50,000 | 3.71 M/s | 2.01 M/s | 1.85× |
| 100,000 | 3.43 M/s | 2.02 M/s | 1.70× |

### End-to-end latency, 100K orders (TSC ticks, 10 reports each)

The benchmarks print these labelled as "cycles" and convert them with a hardcoded
2.494 GHz; both are wrong (AUDIT C6). They are TSC ticks.

| Percentile | SPSC median | SPSC CV | SPSC range | Mutex median | Mutex CV | Mutex range |
|---|---:|---:|---:|---:|---:|---:|
| P50 | 2,672,502 | 5.9% | 2,552,272–3,134,475 | 657,466 | 116.1% | 183,911–5,206,257 |
| P95 | 3,312,450 | 1.3% | 3,206,114–3,370,746 | 3,032,262 | 62.0% | 2,306,934–10,365,969 |
| P99 | 3,354,722 | 1.8% | 3,304,632–3,531,089 | 3,215,196 | 59.8% | 2,479,796–10,534,468 |
| P999 | 3,373,321 | 1.9% | 3,314,222–3,553,722 | 3,262,133 | 59.3% | 2,496,125–10,588,573 |
| Max | 3,378,198 | 1.9% | 3,318,269–3,563,682 | 3,273,772 | 59.1% | 2,503,252–10,595,531 |

Trades produced by the SPSC pipeline: 62155 in every report — identical to
the figure recorded on the VM in `learning_notes/11_pipeline.md`, which confirms the
fixed-seed workload is reproducible across machines.

## What the anchor confirms about the audit

### C1 — end-to-end latency is queue occupancy, on a second machine too

Service time from the 100K throughput run is 29.12 ms / 100,000 = **291 ns** per
order. A saturated 4095-slot ring therefore predicts a P50 wait of about
4095 × 291 ns = **1.19 ms**. Measured P50 is 2,672,502 TSC ticks, about
**1.10 ms** at the ~2.419 GHz estimate above (uncalibrated; used only for this
consistency check). The prediction also overestimates slightly because the
throughput run includes thread start-up. On the VM the same arithmetic gave
1.30 ms predicted against 1.26 ms measured. The mechanism holds on both machines.

### The published P50 comparison does not reproduce

README claims the SPSC pipeline's P50 is 3.8× better than the mutex baseline's.
On this machine, with the same code, the SPSC median P50 is 2,672,502 ticks and the
mutex median P50 is 657,466 ticks — the order is reversed.
This does **not** mean the mutex pipeline is faster: SPSC throughput is still
1.70× higher at 100K. It means the metric measures backlog. The SPSC ring is
bounded and always full, so its wait is a stable ~4095 × service time. The mutex
queue is unbounded and its backlog depends on a producer/consumer scheduling race:
its P50 ranged 183,911–5,206,257 ticks across ten runs (CV 116%). A metric
whose ranking of two implementations can flip from run to run cannot support the
comparison it was used for.

### C3/C4 — the pause pedestal

`BM_AddOrder_NoMatch`, which pauses once per 1,000 iterations, is 39.5 ns.
`BM_AddOrder_FullMatch` (405.4 ns) does no matching work after its first 10,000
iterations (C3), so its figure is essentially the per-iteration Pause/Resume cost
(C4). `BM_AddOrder_SweepLevels/1` is 449.7 ns on the same pedestal. The
incremental cost of sweeping from 1 to 20 levels is (1660.9 − 449.7) / 19 ≈
64 ns per level. The pedestal here is roughly half the ~800 ns seen on
the VM; its size depends on the machine's clock-read cost, which is itself a
reason it must not be inside the timed region.

### C5 — `BM_CancelOrder` times destruction

A cancel is a hash lookup and a pointer store, yet the benchmark reports
44,996 ns. The timed region includes `~OrderBook()`: releasing a hash table
reserved for 65,536 entries plus 1,000 price levels.

## Limits of this measurement

- No kernel-level isolation (`isolcpus`/`nohz_full`). The measurement cores are
  selected and interrupt-steered but the scheduler may still place other tasks
  on them. Pinning is by `taskset` on the whole process; there is no per-thread
  affinity yet, so which pipeline thread lands on which core is not controlled.
- Two IOMMU interrupts (`dmar`) could not be moved off the measurement cores; none
  fired during the run.
- The `mhz_per_cpu` field in each benchmark JSON's `context` differs between
  binaries (2015–2400). Google Benchmark samples it from an idle core at start-up,
  so it is not a record of the frequency during the run. The frequency check above
  and `env_*.json` are the authoritative record.
- `BM_Pipeline_Throughput/10000` (CV 12.8%) and `BM_Mutex_ProducerConsumer/10000`
  (CV 11.2%) are dominated by thread start-up and lock spinning respectively and
  are the least stable figures here.

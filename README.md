# Low-Latency Matching Engine (C++20)

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)

An exchange matching engine in C++20 with a lock-free two-thread pipeline, a custom memory
pool and TSC-based latency measurement. It was built to apply and measure the low-latency
techniques used in trading infrastructure. Every performance figure below comes from a
recorded study on bare metal that the repository's tools can reproduce.

---

## 1. Overview

- **Price-time priority matching** of limit orders and cancels, FIFO within each price level.
- **Lock-free pipeline**: a producer thread feeds orders through an SPSC ring buffer to a
  consumer thread that matches them; a second ring returns finished orders to the producer's
  memory pool. No mutex is taken on the hot path.
- **No per-order allocation of orders or trades**: orders come from the memory pool and trades
  go into a pre-allocated buffer. The order book still allocates when it opens a price level or
  a level's vector grows.
- **Measured against what it replaced**: the order book from before the optimizations and a
  mutex-based pipeline are kept as baselines and recorded on the same machine as the current code.

A methodology audit ([`docs/AUDIT.md`](docs/AUDIT.md)) found defects in the original benchmarks.
They were fixed, and the figures first published from a cloud VM were withdrawn; they are kept at
the tag `v1.0-vm-era`.

---

## 2. Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                          MatchingEngine (C++20)                              │
│                                                                              │
│   Producer Thread                              Consumer Thread               │
│   ─────────────────────────────                ────────────────────────────  │
│   FeedSimulator::generate_into()                                             │
│     │                                                                        │
│     ▼                                                                        │
│   OrderMemoryPool::allocate()                                                │
│     │  O(1), no malloc                                                       │
│     ▼                                                                        │
│   slot->timestamp_ns = RDTSC()                                               │
│     │                                                                        │
│     │   try_push() [release]                                                 │
│     ▼ ── order_queue_ (Producer → Consumer) ──►    try_pop() [acquire]       │
│                                                       │                      │
│                                                       ▼                      │
│                                               OrderBook::add_order_noalloc   │
│                             price-time priority match │                      │
│                                                       ▼                      │
│                                      TradeRingBuffer<4096>                   │
│                                                       │                      │
│                    try_push(filled Order*) [release]  ▼                      │
│    ◄─ return_queue_ (Consumer → Producer)─────────────┘                      │
│   try_pop() [acquire]                                                        │
│     │                                                                        │
│     ▼                                                                        │
│   OrderMemoryPool::deallocate()                                              │
│        only the producer thread touches the pool                             │
└──────────────────────────────────────────────────────────────────────────────┘
```

| Component | Implementation | Why |
|-----------|----------------|-----|
| Order queues | `SPSCRingBuffer<T, N>` | Acquire/release ordering, no lock or system call; `head_` and `tail_` on separate cache lines |
| Memory | `OrderMemoryPool`, a free list | O(1) allocate and free, used only by the producer; ownership moves through the two queues |
| Price levels | `std::map` of `PriceLevel` (vector + head cursor) | Best price at `begin()`; the head cursor pops from the front without shifting elements |
| Order index | `absl::flat_hash_map` | Order ID to resting order, for cancels |
| Trades | `TradeRingBuffer<4096>` | Pre-allocated, replaces the `std::vector<Trade>` that `add_order` returns |
| Latency | RDTSC between two `lfence` | A timestamp read without a system call; values are TSC ticks |

---

## 3. Scope and Limitations

- **Matching core only.** Trades are meant to be taken by a downstream consumer that stores or
  publishes them. `TradeRingBuffer` stands in for that consumer: it keeps the most recent trades
  and overwrites older ones. The figures cover matching and writing the trade record, not
  delivering trades.
- **Synthetic order flow.** `FeedSimulator` draws prices around a fixed base price and picks the
  target of a cancel at random among earlier order IDs, so many cancels find no resting order.
  The results compare implementations under this workload; they do not describe behaviour on
  real market data.
- **Trusted input.** The engine assumes that upstream checks have run: live order IDs are unique
  and quantities are positive. It does not check them itself.
- **Fixed capacity.** The memory pool is sized for the benchmark workloads. When it runs out, the
  producer waits for orders to come back.
- **One machine.** All figures come from one CPU model and describe that machine.

---

## 4. Results

All numbers come from one recording,
[`docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/`](docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/),
of the code at commit `999c956` (recorded as `46cf493` before the history was rewritten; see
[`docs/evidence/`](docs/evidence/README.md#commit-ids)).

> **Machine**: Intel Core i5-1135G7, bare metal, cores held at the 2.4 GHz base frequency, turbo
> off, SMT siblings offline; kernel 7.0.0-34-generic; GCC 15.2.0, Release (`-O2`).
> **Method**: each benchmark program was launched 32 times, with 6 repetitions per benchmark and
> launch and the first left out as warm-up. Each value is the median of the 32 launch medians.
> [`docs/BENCHMARKING.md`](docs/BENCHMARKING.md) describes how a recording is made and checked.

### 4.1 Order Book, Before and After the Optimizations (one thread)

`v1` is the order book from before the optimizations (`std::mutex`, a `std::deque` per level,
`std::unordered_map` index), kept in `benchmarks/order_book_v1.h`; `v3` is the current one. Both
run through the same benchmark code.

| Benchmark | Timed work per iteration | v1 | v3 | v1 / v3 |
|-----------|--------------------------|---:|---:|:-------:|
| `BM_AddOrder_NoMatch` | Add a buy order that does not cross 100 ask levels | 50.3 ns | 38.2 ns | 1.32× |
| `BM_AddOrder_FullMatch` | Add an order that fills the one order of the best level | 179.6 ns | 140.5 ns | 1.28× |
| `BM_AddOrder_SweepLevels/1` | Add an order that takes 1 level | 178.7 ns | 138.6 ns | 1.29× |
| `BM_AddOrder_SweepLevels/5` | Add an order that takes 5 levels | 762.0 ns | 484.7 ns | 1.57× |
| `BM_AddOrder_SweepLevels/10` | Add an order that takes 10 levels | 1,431 ns | 898.4 ns | 1.59× |
| `BM_AddOrder_SweepLevels/20` | Add an order that takes 20 levels | 2,855 ns | 1,816 ns | 1.57× |
| `BM_CancelOrder` | Cancel a resting order that is alone at its level | 277.9 ns | 206.3 ns | 1.35× |
| `BM_MixedWorkload` | Replay 1,000,000 generated orders (10% cancels) into an empty book | 299.8 ms | 214.6 ms | 1.40× |

Launch medians vary by a coefficient of variation (CV) of 0.2% to 1.7%. The exception is v3's
`BM_AddOrder_NoMatch`, where about one repetition in five runs 1.2 to 2.5 times slower with many
page faults; the cause is open and the median absorbs it. In that benchmark v3's `add_order`
reserves room for trades before matching, so its timed region holds one `malloc`/`free` pair per
order although nothing trades.

The pipeline's consumer calls `add_order_noalloc`, which writes trades into the pre-allocated
buffer. Replaying one stream of 10,000 generated orders (20% cancels) through each interface of
the v3 book:

| Benchmark | Interface | Time per order |
|-----------|-----------|---------------:|
| `BM_SingleThread_Baseline_AddOrder` | `add_order` and `cancel_order` | 138.5 ns |
| `BM_SingleThread_Baseline` | `add_order_noalloc` | 126.0 ns |

This compares the two interfaces as a whole, including what the compiler inlines into each.

### 4.2 Pipeline Throughput

Both pipelines run a producer and a consumer thread, each pinned to its own core. The mutex
pipeline (`bench_baseline_mutex`) is the earlier system as a whole: a queue guarded by
`std::mutex` and `std::condition_variable`, the v1 order book, and a heap allocation per order.
The speedup compares two systems, not two queues.

| Orders per run | Mutex pipeline | SPSC pipeline | Speedup |
|---------------:|:--------------:|:-------------:|:-------:|
| 10K  | 6.75 ms (1.48 M orders/s) | 2.46 ms (4.06 M orders/s) | 2.74× |
| 50K  | 33.1 ms (1.51 M orders/s) | 13.2 ms (3.79 M orders/s) | 2.51× |
| 100K | 65.7 ms (1.52 M orders/s) | 28.6 ms (3.50 M orders/s) | **2.30×** |

Times are wall-clock per run, thread start-up included. Launch medians vary by a CV of 2.1% to
4.9%; at 100K orders the speedup lies between 2.18× and 2.52× in all 32 launches.

### 4.3 Latency at 100K Orders

A separate run records two times per order: **queue time**, from the producer's timestamp until
the consumer takes the order out of the queue, and **service time**, from then until matching is
done. Values are TSC ticks, converted with the TSC frequency calibrated during the recording,
2.419 GHz.

| Pipeline, quantity | P50 | P99 | P99.9 | CV of P50 over launches |
|--------------------|----:|----:|------:|------------------------:|
| SPSC, service time  | 506 ticks (209 ns) | 1,412 ticks (584 ns) | 2,358 ticks (975 ns) | 1.2% |
| Mutex, service time | 616 ticks (255 ns) | 1,889 ticks (781 ns) | 6,266 ticks (2.59 µs) | 0.2% |
| SPSC, queue time    | 3.52 M ticks (1.46 ms) | 4.04 M ticks (1.67 ms) | 4.05 M ticks (1.68 ms) | 3.3% |
| Mutex, queue time   | 7.29 M ticks (3.01 ms) | 16.8 M ticks (6.92 ms) | 16.9 M ticks (7.00 ms) | 26% |

Service time is what one order costs the consumer. Queue time is measured at full load: the
producer outruns the consumer, so each order waits behind a full queue, the 4,095 slots of the
SPSC ring or an unbounded mutex queue. The fences around each reading slow this run's consumer by
about 24% compared with §4.2.

### 4.4 Queue Microbenchmarks

`bench_spsc_ring_buffer` moves items from a producer thread to a consumer thread. Each launch
measures every benchmark on 16 freshly allocated memory regions, in both assignments of the two
threads to the two cores.

| Benchmark | Items | Time per item |
|-----------|------:|---------------|
| `BM_SPSC_ProducerConsumer` (int64) | 100K | 8.13 ns in the fast state, 56.6 ns in the slow state |
| `BM_SPSC_ProducerConsumer` (int64) | 10K  | 9.74 ns in the fast state, 35.4 ns in the slow state |
| `BM_SPSC_Order_ProducerConsumer` (64-byte `Order`) | 10K | median 54.9 ns, quartiles 52.0 to 60.3 ns |
| `BM_Mutex_ProducerConsumer` (int64, mutex queue) | 10K  | median 300.4 ns, quartiles 286 to 318 ns |
| `BM_Mutex_ProducerConsumer` (int64, mutex queue) | 100K | median 303.2 ns, quartiles 287 to 322 ns |

The int64 ring runs in a fast or a slow state, set by the memory region and the thread
assignment rather than by noise. The physical structure of the last-level cache's slices slightly
changes the speed of the producer and the consumer, and this splits the ring into the two states.
Which physical structure is responsible, and how it produces the two states, needs further
investigation. The studies so far are kept at the tags `archive/queue-regimes-2026-10` and
`archive/queue-mechanism-2026-10`.

---

## 5. Build, Test and Run

Requires GCC 11+ (verified on GCC 15.2), CMake 3.20+ (verified on CMake 4.2) and internet access
on the first configure, which fetches GoogleTest and Google Benchmark. Installing `libabsl-dev`
is optional and saves building Abseil from source in Release.

```bash
sudo apt install cmake libabsl-dev

# Release, for benchmarks
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j$(nproc)

# Debug, for tests (ASan + UBSan)
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug --output-on-failure

# Demo
./build/release/matching_engine
```

Debug builds always compile Abseil from source and enable an explicit list of UBSan checks;
`CMakeLists.txt` explains why. `ctest` runs the 63 GoogleTest cases, the tests of the measurement
scripts, the benchmarks' own consistency checks and the TSC calibration; all pass in both
configurations.

A study is recorded with the measurement scripts, which put the machine into a measurement mode
(pinned frequency, SMT siblings offline, interrupts steered away) for the duration:

```bash
sudo scripts/env/install_bench_env.sh          # once per machine
scripts/record/bench_record.py --label control
scripts/report/bench_report.py docs/evidence/<cpu>/<study>
```

[`docs/BENCHMARKING.md`](docs/BENCHMARKING.md) describes the workflow and the checks made for
every launch.

---

## 6. Repository Layout

```
include/, src/   The engine: order, order book, SPSC ring, memory pool, trade buffer, pipeline
tests/           GoogleTest suites, run by ctest
benchmarks/      Google Benchmark programs, with the v1 order book and the mutex pipeline baselines
scripts/         Measurement tooling: env/ (measurement mode), record/, report/, diagnostics/
docs/            AUDIT.md, BENCHMARKING.md, and evidence/ with the recording behind §4
```

---

## 7. Roadmap

The long-term goal is to drive the engine with real order and market data instead of the
synthetic feed, and to measure it on that flow.

---

## License

MIT. See [`LICENSE`](LICENSE).

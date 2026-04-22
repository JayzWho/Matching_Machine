# Low-Latency Matching Engine (C++20)

A high-performance exchange matching engine built in C++20, featuring a lock-free SPSC pipeline,
custom memory pool, and end-to-end RDTSC latency measurement. Designed to demonstrate
low-latency systems programming techniques applicable to HFT and quantitative trading infrastructure.

---

## 1. Project Overview

Modern electronic exchanges must process millions of orders per second with sub-millisecond
latency. This project implements the core matching engine component from scratch:

- **Price-time priority matching** (FIFO within each price level) with full Limit/Cancel order support
- **Lock-free producer-consumer pipeline**: orders flow from a feed simulator through an SPSC ring
  buffer into the order book, with no mutexes on the hot path
- **Zero heap-allocation hot path**: a custom memory pool + `TradeRingBuffer` eliminate all
  `malloc`/`free` calls during steady-state processing
- **Rigorous measurement**: RDTSC-based latency recorder with P50/P95/P99/P999 reporting;
  a mutex baseline benchmark provides a quantified comparison of the optimizations

The project evolved through a documented, data-driven optimization cycle:
mutex baseline → cache-friendly data structures → lock-free SPSC → zero-alloc hot path.
Every decision is backed by `perf` flame-graph evidence and before/after Google Benchmark numbers.

For a detailed Chinese write-up (architecture + rationale + perf analysis), see `docs/project_overview.md`.

---

## 2. System Architecture

### 2.1 High-Level Pipeline

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                          MatchingEngine (C++20)                              │
│                                                                              │
│   Producer Thread                              Consumer Thread               │
│   ─────────────────────────────                ────────────────────────────  │
│   FeedSimulator::generate_random()                                           │
│     │                                                                        │
│     ▼                                                                        │
│   OrderMemoryPool::allocate()                                                │
│     │  O(1), no malloc                                                       │
│     ▼                                                                        │
│   slot->timestamp_ns = RDTSC()                                               │
│     │  latency measurement start                                             │
│     │                                                                        │
│     │   try_push() [release]                                                 │
│     ▼ ── order_queue_ (Producer → Consumer) ──►    try_pop() [acquire]       │
│                                                       │                      │
│                                                       ▼                      │
│                                               OrderBook::add_order_noalloc   │
│                             price-time priority match │                      │
│                                                       ▼                      │
│                                      TradeRingBuffer<4096>                   │
│                          push_trade()  (array write)  │                      │
│                                                       │                      │
│                    try_push(filled Order*) [release]  ▼                      │
│    ◄─ return_queue_ (Consumer → Producer)─────────────┘                      │
│   try_pop() [acquire]                                                        │
│     │                                                                        │
│     ▼                                                                        │
│   OrderMemoryPool::deallocate()                                              │
│     │  exclusively on producer thread                                        │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Two SPSC queues** form the backbone:
- `order_queue_` — Producer → Consumer (forward data flow)
- `return_queue_` — Consumer → Producer (return filled `Order*` for pool reclamation)

`OrderMemoryPool` is accessed **exclusively by the producer thread** (allocate + deallocate),
with memory ownership transferred through the two SPSC queues' `acquire`/`release` semantics.
No mutex is needed anywhere in steady-state processing.

### 2.2 OrderBook Data Structure

```
bid_levels_   std::map<price, PriceLevel, std::greater<>>
               │
               └── PriceLevel { std::vector<Order*> orders; size_t head; }
                    head游标 (lazy pop_front, no element shift)
                    compact() when holes > 50%

ask_levels_   std::map<price, PriceLevel>  (ascending)

order_index_  absl::flat_hash_map<order_id, Order*>
               reserved(65536) — zero rehash during benchmark
```

### 2.3 Order Lifecycle

```
pool.allocate()
    │ placement new, O(1)
    ▼
[Producer: fill fields, write RDTSC timestamp]
    │ try_push(slot)   [release]
    ▼
[Consumer: try_pop]   [acquire — all writes visible]
    │
    ├─→ immediate fill / cancel → return_queue_.try_push → pool.deallocate
    │
    └─→ resting (added to PriceLevel)
              │
              ▼  matched by future incoming order
         deallocator(resting) → return_queue_.try_push → pool.deallocate
```

---

## 3. Core Technologies

| Component | Technology | Why |
|-----------|-----------|-----|
| Order queue | `SPSCRingBuffer<T, N>` (custom) | No mutex, no syscall; acquire/release memory order; `head_`/`tail_` each `alignas(64)` eliminates false sharing between producer/consumer cores |
| Memory management | `OrderMemoryPool` (custom free-list) | `malloc` latency is 50–500 ns with unpredictable spikes; pool gives stable < 10 ns allocations |
| Trade collection | `TradeRingBuffer<4096>` (custom) | Replaces per-call `std::vector<Trade>` heap alloc; eliminates frequent heap allocations beyond `map` reallocation (identified by `perf` flame-graph) |
| Order index | `absl::flat_hash_map` (Swiss Table) | Open-addressing; SIMD batch probe; reduced hash-map CPU overhead from ~32% → ~12% (perf measured) |
| Price levels | `std::map` + `PriceLevel` vector + head cursor | `std::map::begin()` = best price in O(1); vector head cursor replaces `deque` for cache-friendly sequential access |
| Latency measurement | RDTSC (`__rdtsc`) + P-quantile sort | `std::chrono` overhead ≈ 50–200 ns; RDTSC ≈ 5–20 cycles. Cross-thread latency via `timestamp_ns` field; safe under SPSC acquire/release |
| Build | CMake + FetchContent | Google Benchmark, Google Test, abseil-cpp pulled automatically |
| Profiling | `perf record -g --cpu-clock` + FlameGraph | Hardware PMU unavailable on cloud VM; cpu-clock software sampling used for function-level hotspot identification |

---

## 4. Performance & Results

> **Test environment**: Tencent Cloud Ubuntu VM, 2 vCPU @ 2494 MHz, Release (-O2)  
> **Run command**: `taskset -c 0,1 ./build/release/bench_matching_engine`  
> **Note**: Hardware PMU counters (cycles, IPC, cache-miss) unavailable in virtualized environment.
> Latency reported in RDTSC cycles, converted at 2.494 GHz.

### 4.1 OrderBook Microbenchmark — Optimization Progression

| Benchmark | Baseline (mutex + deque + unordered_map) | Optimized (v3: no-mutex + vector+head + flat_hash_map) | Improvement |
|-----------|:---:|:---:|:---:|
| BM_AddOrder_NoMatch | 83.5 ns | 46.4 ns | **+44%** |
| BM_CancelOrder | 60,456 ns | 26,659 ns | **+56%** |
| BM_AddOrder_SweepLevels/20 | 3,093 ns | 2,297 ns | **+26%** |
| BM_MixedWorkload throughput | 3.87 M ops/s | 6.19 M ops/s | **+60%** |
| BM_MixedWorkload latency stddev | 5.49 ms | 2.91 ms | **CV 2.1% → 1.8%** (more stable) |

`absl::flat_hash_map` reduced hash-map CPU overhead from ~32% → ~12% (perf flame-graph measured).

### 4.2 Pipeline Throughput (real-time, orders/s)

| Scale | Mutex baseline | SPSC optimized | Speedup |
|-------|:--------------:|:--------------:|:-------:|
| 10K   | 1.22 M/s       | 1.75 M/s       | 1.43×   |
| 50K   | 1.17 M/s       | 3.23 M/s       | 2.76×   |
| 100K  | 1.19 M/s       | 3.15 M/s       | **2.65×** |

Mutex throughput plateaus at ~1.19 M/s regardless of scale — `condition_variable` kernel
scheduling overhead saturates the consumer. SPSC stabilizes at 3.15–3.23 M/s.

### 4.3 End-to-End Latency Distribution (100K orders, RDTSC)

| Percentile | Mutex baseline | SPSC optimized | Improvement |
|:----------:|:--------------:|:--------------:|:-----------:|
| P50        | ~4.77 ms       | ~1.26 ms       | **3.8×**    |
| P95        | ~11.88 ms      | ~2.03 ms       | 5.9×        |
| P99        | ~12.59 ms      | ~2.49 ms       | **5.1×**    |
| P999       | ~12.66 ms      | ~2.84 ms       | 4.5×        |
| Max        | ~12.67 ms      | ~2.84 ms       | 4.5×        |

SPSC P999 ≈ Max (2.84 ms) indicates a tight, well-bounded tail with no severe outliers.
On bare-metal hardware, SPSC P50 typically reaches 200–800 ns; VM hypervisor scheduling
inflates these numbers but preserves the relative improvement ratio.

### 4.4 noalloc Interface Improvement (single-thread)

| Interface | Throughput | Notes |
|-----------|:----------:|-------|
| `add_order` (vector<Trade> return) | 6.19 M ops/s | baseline after v3 data-structure opts |
| `add_order_noalloc` | **8.04 M ops/s** | **+30%** — eliminates per-call heap alloc for trades |

Consistent with perf report: `_int_malloc` + `_int_free` ≈ 20% of CPU time in v3 profile
(including trade vector alloc, order construction, and map rehashing).

---

## 5. Build & Run

### Prerequisites

- GCC 11+ or Clang 14+ (C++20)
- CMake 3.20+
- Internet access (FetchContent downloads Google Benchmark, GTest, abseil-cpp)

### Build

```bash
# Release (for benchmark / latency measurements)
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j$(nproc)

# Debug (for tests, AddressSanitizer enabled)
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j$(nproc)
```

### Run Tests

```bash
cd build/debug && ctest --output-on-failure
```

Current test coverage: 33 GTest cases across OrderBook, SPSC Ring Buffer, MemoryPool,
FeedSimulator, and MatchingEngine (including 13 integration tests covering memory safety,
deallocator callbacks, and concurrent correctness).

### Run Benchmarks

```bash
# SPSC pipeline (bind to 2 cores to stabilize RDTSC cross-thread measurement)
taskset -c 0,1 ./build/release/bench_matching_engine

# Mutex baseline (symmetric binding for fair comparison)
taskset -c 0,1 ./build/release/bench_baseline_mutex

# OrderBook microbenchmarks (single-thread, bind to core 0)
taskset -c 0 ./build/release/bench_order_book
```

---

## 6. Repository Layout

```
Matching_Machine/
├── include/
│   ├── order.h               # Order struct: alignas(64), 64-byte cache-line fit
│   ├── order_book.h          # OrderBook: PriceLevel + absl::flat_hash_map
│   ├── spsc_ring_buffer.h    # Lock-free SPSC queue (acquire/release, false-sharing free)
│   ├── memory_pool.h         # Free-list pool: O(1) alloc/dealloc, placement new
│   ├── trade_ring_buffer.h   # Pre-allocated trade buffer (no heap alloc)
│   ├── matching_engine.h     # SPSC dual-thread pipeline
│   ├── feed_simulator.h      # Synthetic order feed (normal-dist price walk + cancel ratio)
│   └── latency_recorder.h    # RDTSC-based P50/P99/P999 recorder
├── src/
│   ├── order_book.cpp
│   ├── matching_engine.cpp
│   ├── feed_simulator.cpp
│   └── main.cpp
├── benchmarks/
│   ├── bench_order_book.cpp       # OrderBook microbenchmarks (before/after optimization)
│   ├── bench_matching_engine.cpp  # SPSC pipeline end-to-end benchmark
│   └── bench_baseline_mutex.cpp   # Mutex baseline (self-contained, does not touch src/)
├── tests/
│   ├── test_order_book.cpp
│   ├── test_spsc_ring_buffer.cpp
│   ├── test_memory_pool.cpp
│   └── test_matching_engine.cpp
├── docs/
│   └── snapshots/                 # Pre-optimization snapshots for diff reference
└── CMakeLists.txt
```


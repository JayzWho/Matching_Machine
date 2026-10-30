# Low-Latency Matching Engine (C++20)

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)

A high-performance exchange matching engine built in C++20, featuring a lock-free SPSC pipeline,
custom memory pool, and RDTSC latency measurement. Designed to demonstrate
low-latency systems programming techniques applicable to HFT and quantitative trading infrastructure.

---

## 1. Project Overview

Modern electronic exchanges must process millions of orders per second with sub-millisecond
latency. This project implements the core matching engine component from scratch:

- **Price-time priority matching** (FIFO within each price level) with full Limit/Cancel order support
- **Lock-free producer-consumer pipeline**: orders flow from a feed simulator through an SPSC ring
  buffer into the order book, with no mutexes on the hot path
- **No per-order allocation of orders or trades**: orders come from a custom memory pool and
  trades go into a pre-allocated `TradeRingBuffer`; the order book still allocates when it
  opens a price level or a level's vector grows
- **Measured against what it replaced**: the order book from before the optimizations and a
  mutex-based pipeline are kept as benchmark baselines and recorded on the same machine as
  the current code; latency is reported as queue time and service time, in TSC ticks

The project evolved through an optimization cycle:
mutex baseline → cache-friendly data structures → lock-free SPSC → allocation-free orders and trades.
A methodology audit ([`docs/AUDIT.md`](docs/AUDIT.md)) found defects in the benchmarks; they were
fixed, and §4 reports the fixed benchmarks on bare metal.

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
               reserve(65536) at construction
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
| Memory management | `OrderMemoryPool` (custom free-list) | O(1) allocate and free on the producer thread, with no `malloc` and no lock per order |
| Trade collection | `TradeRingBuffer<4096>` (custom) | Replaces the `std::vector<Trade>` that `add_order` allocates for every call |
| Order index | `absl::flat_hash_map` (Swiss Table) | Open addressing with SIMD group probing; 65,536 slots reserved up front |
| Price levels | `std::map` + `PriceLevel` vector + head cursor | `std::map::begin()` = best price in O(1); vector head cursor replaces `deque` for cache-friendly sequential access |
| Latency measurement | RDTSC between two `lfence` + P-quantile sort | Reads the timestamp counter without a system call, and the fences keep each reading at its place in the code. Queue time subtracts a producer-core reading from a consumer-core reading: acquire/release makes the timestamp visible, and the comparison relies on the cores' TSCs being synchronised, which the kernel checks at boot |
| Build | CMake + FetchContent | Google Benchmark and GoogleTest fetched at configure time; Abseil resolved system-first with a pinned source fallback |

---

## 4. Performance & Results

Every number in this section comes from one recording, the current control in
[`docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/`](docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/),
and describes the code at commit `46cf493`.

> **Machine**: Intel Core i5-1135G7, bare metal, cores held at the 2.4 GHz base frequency with
> turbo off and SMT siblings offline; Ubuntu with kernel 7.0.0-34-generic; GCC 15.2.0, Release (`-O2`).
> **Method**: each benchmark program was launched 32 times, with 6 repetitions of every benchmark
> per launch and the first left out as a warm-up. A launch is represented by the median of its
> repetitions, and each value below is the median of the 32 launch medians.
> [`docs/BENCHMARKING.md`](docs/BENCHMARKING.md) describes how a recording is made and checked.

The figures first published in this section were measured on a cloud VM with benchmarks that a
methodology audit ([`docs/AUDIT.md`](docs/AUDIT.md)) later found defective. They are kept at the
tag `v1.0-vm-era`.

### 4.1 Order Book: Before and After the Optimizations (one thread)

`v1` is the order book from before the optimizations (`std::mutex`, a `std::deque` per price
level, an `std::unordered_map` index), kept in `benchmarks/order_book_v1.h`. `v3` is the current
one. `bench_order_book` runs both through the same benchmark code.

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

Launch medians vary by a coefficient of variation (CV) of 0.2% to 1.7%, with one exception:
about one repetition in five of v3's `BM_AddOrder_NoMatch` runs 1.2 to 2.5 times slower, together
with many page faults. The cause is not established; the median absorbs it, and the
[report of the recording](docs/evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/REPORT.md)
describes it. In this benchmark v3's `add_order` reserves room for trades before it matches, so
its timed region holds one `malloc`/`free` pair per order although nothing trades; v1 allocates
only when a trade occurs.

**`add_order_noalloc`.** The pipeline's consumer calls `add_order_noalloc`, which writes trades
into a pre-allocated `TradeRingBuffer` instead of returning a `std::vector<Trade>`.
`bench_matching_engine` replays one stream of 10,000 generated orders (20% cancels) through each
interface of the v3 book:

| Benchmark | Interface | Time per order |
|-----------|-----------|---------------:|
| `BM_SingleThread_Baseline_AddOrder` | `add_order` and `cancel_order` | 138.5 ns |
| `BM_SingleThread_Baseline` | `add_order_noalloc` | 126.0 ns |

`add_order_noalloc` takes 12.5 ns (9%) less per order. This compares the two interfaces as a
whole, including what the compiler inlines into each; it cannot separate the allocation from the
rest of the call.

### 4.2 Pipeline Throughput

Both pipelines run a producer thread that generates orders and a consumer thread that matches
them, each pinned to its own core. The SPSC pipeline is the current engine. The mutex pipeline
(`bench_baseline_mutex`) is the earlier system as a whole and differs in three ways at once: a
queue guarded by `std::mutex` and `std::condition_variable` instead of two SPSC rings, the v1
order book, and a heap allocation (`make_shared`) per order instead of the memory pool. The
speedup therefore compares two systems, not two queues.

| Orders per run | Mutex pipeline | SPSC pipeline | Speedup |
|---------------:|:--------------:|:-------------:|:-------:|
| 10K  | 6.75 ms (1.48 M orders/s) | 2.46 ms (4.06 M orders/s) | 2.74× |
| 50K  | 33.1 ms (1.51 M orders/s) | 13.2 ms (3.79 M orders/s) | 2.51× |
| 100K | 65.7 ms (1.52 M orders/s) | 28.6 ms (3.50 M orders/s) | **2.30×** |

Times are wall-clock time per run, thread start-up included. Launch medians vary by a CV of 2.1%
to 4.9%, and at 100K orders the speedup lies between 2.18× and 2.52× in every one of the 32
rounds.

### 4.3 Latency at 100K Orders

A separate run of each pipeline records two times per order: **queue time**, from the producer's
timestamp to the moment the consumer takes the order out of the queue, and **service time**, from
then until matching is done. Values are TSC ticks; the nanoseconds use the TSC frequency
calibrated during the recording, 2.419 GHz.

| Pipeline, quantity | P50 | P99 | P99.9 | CV of P50 over launches |
|--------------------|----:|----:|------:|------------------------:|
| SPSC, service time  | 506 ticks (209 ns) | 1,412 ticks (584 ns) | 2,358 ticks (975 ns) | 1.2% |
| Mutex, service time | 616 ticks (255 ns) | 1,889 ticks (781 ns) | 6,266 ticks (2.59 µs) | 0.2% |
| SPSC, queue time    | 3.52 M ticks (1.46 ms) | 4.04 M ticks (1.67 ms) | 4.05 M ticks (1.68 ms) | 3.3% |
| Mutex, queue time   | 7.29 M ticks (3.01 ms) | 16.8 M ticks (6.92 ms) | 16.9 M ticks (7.00 ms) | 26% |

Service time is what one order costs the consumer. Queue time is measured at full load: the
producer outruns the consumer, so each order waits behind a full queue. In the SPSC pipeline that
is the 4,095 orders of the full ring; the mutex queue has no bound, so its length, and the queue
time with it, moves from launch to launch. Only this run reads the TSC for every order, and the
fences around each reading slow its consumer by about 24%, so the queue time is that of a slower
consumer than in §4.2.

### 4.4 Queue Microbenchmarks

`bench_spsc_ring_buffer` moves items from a producer thread to a consumer thread. Each launch
measures every benchmark on 16 freshly allocated memory regions in both assignments of the two
threads to the two cores: 32 instances per benchmark and launch.

| Benchmark | Items | Time per item |
|-----------|------:|---------------|
| `BM_SPSC_ProducerConsumer` (int64) | 100K | 8.13 ns in the fast state, 56.6 ns in the slow state |
| `BM_SPSC_ProducerConsumer` (int64) | 10K  | 9.74 ns in the fast state, 35.4 ns in the slow state |
| `BM_SPSC_Order_ProducerConsumer` (64-byte `Order`) | 10K | median 54.9 ns, quartiles 52.0 to 60.3 ns |
| `BM_Mutex_ProducerConsumer` (int64, mutex queue) | 10K  | median 300.4 ns, quartiles 286 to 318 ns |
| `BM_Mutex_ProducerConsumer` (int64, mutex queue) | 100K | median 303.2 ns, quartiles 287 to 322 ns |

The int64 ring runs in one of two states, a property of the queue rather than noise. When the
slice of the last-level cache that serves the ring's `tail_` index is the slice of the producer's
core, the producer slows down and the consumer speeds up, so the consumer catches up; from then on
the two sides contend for the cache line being written, at about seven times the cost per item.
The memory region sets the slice and the thread assignment sets the core: 29% and 22% of the
instances were slow in the two assignments, and no region was slow in both. The Order ring has no
such amplifier, and the same placement makes it about 20% slower, hence its median and quartiles.
The investigation is kept at the tags `archive/queue-regimes-2026-10` and
`archive/queue-mechanism-2026-10`.

---

## 5. Build & Run

### Prerequisites

- GCC 11+ (C++20) — verified on GCC 15.2
- CMake 3.20+ — verified on CMake 4.2
- Internet access on first configure (GoogleTest and Google Benchmark are fetched
  by `FetchContent`)

```bash
# Debian / Ubuntu
sudo apt install cmake libabsl-dev
```

`libabsl-dev` is optional but recommended: Abseil is resolved system-first, and
without it CMake falls back to building Abseil 20260107.0 from source, which adds
several minutes to the first configure.

### Build

```bash
# Release (for benchmarks and latency measurements)
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j$(nproc)

# Debug (for tests; ASan + UBSan enabled)
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j$(nproc)
```

> **Debug builds always compile Abseil from source**, even when `libabsl-dev` is
> installed. Abseil's SwissTable changes its small-object-optimization layout under
> AddressSanitizer, so sanitizer-instrumented code linked against a non-instrumented
> system `libabsl.so` aborts inside Abseil at runtime. The first Debug configure is
> therefore slower. See the comments in `CMakeLists.txt`.

### Run Tests

```bash
ctest --test-dir build/debug --output-on-failure
```

63 GTest cases across five targets — `test_order_book` (14), `test_matching_engine`
(21, covering the SPSC pipeline, `TradeRingBuffer` and the no-alloc order-book path),
`test_spsc_ring_buffer` (10, including two concurrent producer/consumer tests),
`test_memory_pool` (9) and `test_thread_placement` (9). All pass under both configurations.

The same command runs the six tests of the measurement scripts, which need no
build and no root, and three checks of the benchmark programs: the single-threaded
benchmarks' own consistency checks and the TSC calibration. `ctest -L scripts`
runs only the script tests, `ctest -LE scripts` everything else.

### Known toolchain constraint

Under GCC 15, UBSan's `null` and `function` sub-checks are incompatible with Abseil's
SwissTable: both break constant-expression evaluation of a function-pointer comparison
in `hash_policy_traits`, and `absl::flat_hash_map` fails to compile. The Debug
configuration therefore enumerates the remaining UBSan checks explicitly rather than
using `-fsanitize=undefined`; GCC does not support removing individual checks from that
group. Null-pointer dereferences remain covered by ASan. Reproduced against both Abseil
20260107 (Ubuntu package) and 20260817.0 (upstream), so this is not a packaging issue.

### Run Benchmarks

```bash
# Once per machine: install the measurement-mode script. It prints the sudoers
# rule that lets `setup` and `restore` run without a password.
sudo scripts/env/install_bench_env.sh

# Record a study, then print its tables. The recorder enters measurement mode
# (pinned frequency, SMT siblings offline, device interrupts steered away),
# launches every benchmark binary repeatedly, and leaves measurement mode.
scripts/record/bench_record.py --label control
scripts/report/bench_report.py docs/evidence/<cpu>/<study>
```

A binary can also be started by hand for a quick look, for example
`./build/release/bench_order_book`, but such a run is not evidence.
[`docs/BENCHMARKING.md`](docs/BENCHMARKING.md) describes the workflow and what is
checked for every launch; [`docs/evidence/`](docs/evidence/README.md) holds the
recorded studies.

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
│   ├── latency_recorder.h    # RDTSC-based P50/P99/P999 recorder
│   └── thread_placement.h    # Threads pinned to a CPU from their creation
├── src/
│   ├── order_book.cpp
│   ├── matching_engine.cpp
│   ├── feed_simulator.cpp
│   └── main.cpp
├── benchmarks/
│   ├── bench_order_book.cpp       # OrderBook microbenchmarks, each on the v1 and the v3 book
│   ├── bench_spsc_ring_buffer.cpp # Ring buffer microbenchmarks, with a mutex queue for comparison
│   ├── bench_matching_engine.cpp  # SPSC pipeline throughput and latency, single-thread baseline
│   ├── bench_baseline_mutex.cpp   # Mutex baseline pipeline (does not use the engine in src/)
│   └── order_book_v1.h            # The order book from before the optimizations
├── scripts/                       # Measurement tooling; each test sits next to its script
│   ├── env/                       # Measurement mode: enter/leave, install, record the environment
│   ├── record/                    # Record a study, or a matrix of diagnostic conditions
│   ├── report/                    # Turn a recorded study into tables
│   └── diagnostics/               # Tools that alter a launch for a diagnostic condition
├── tests/
│   ├── test_order_book.cpp
│   ├── test_spsc_ring_buffer.cpp
│   ├── test_memory_pool.cpp
│   ├── test_matching_engine.cpp
│   └── test_thread_placement.cpp
├── docs/
│   ├── AUDIT.md                   # Methodology audit of the benchmarks and its outcome
│   ├── BENCHMARKING.md            # How measurements are made and checked
│   └── evidence/                  # Recorded studies, including the control behind §4
└── CMakeLists.txt
```


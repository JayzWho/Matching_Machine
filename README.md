# Low-Latency Matching Engine (C++20)

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)

An exchange matching engine in C++20, built to apply and measure the low-latency techniques used
in trading infrastructure. On an Intel Core i5-1135G7 it processes 3.5 million orders per second,
2.3 times the mutex-based design it replaced.

---

## 1. Highlights

- **Price-time priority matching** of limit orders and cancels, FIFO within each price level.
- **Lock-free pipeline**: a producer thread feeds orders through an SPSC ring buffer to a
  consumer thread that matches them; a second ring returns finished orders to the producer's
  memory pool. No mutex is taken on the hot path.
- **No per-order allocation of orders or trades**: orders come from the memory pool and trades
  go into a pre-allocated buffer. The order book still allocates when it opens a price level or
  a level's vector grows.
- **Reproducible measurement on bare metal**: the earlier design is kept as a baseline and
  measured on the same machine as the current code. Scripts put the machine into a measurement
  mode, launch each benchmark program 32 times, check every launch for throttling and
  interrupts, and calibrate the TSC.

---

## 2. Performance

| Measure | Earlier design | Current | Ratio |
|---------|---------------:|--------:|:-----:|
| Pipeline throughput, 100K orders, two threads | 1.52 M orders/s | 3.50 M orders/s | 2.30× |
| Order book alone, replay of 1M generated orders, one thread | 299.8 ms | 214.6 ms | 1.40× |
| Service time per order in the pipeline, P50 / P99.9 | 255 ns / 2.59 µs | 209 ns / 975 ns | |

The earlier design passes orders through a queue guarded by `std::mutex` and
`std::condition_variable`, allocates every order on the heap, and matches with an order book that
takes a mutex and keeps a `std::deque` per price level and a `std::unordered_map` index. The
pipeline row therefore compares two whole systems; the order book row compares the two books
alone. The seven benchmarks that time a single order book operation, from adding one order to
sweeping 20 price levels or cancelling, improve by 1.28× to 1.59×.

Each benchmark program was launched 32 times, and each value is the median of the launch medians;
at 100K orders the pipeline speedup lies between 2.18× and 2.52× in every launch. Throughput is
wall-clock time per run, thread start-up included. Service time is the time the consumer spends
on one order, from taking it out of the queue until matching is done; it is read with the TSC and
converted with the TSC frequency calibrated during the recording, 2.419 GHz.

> **Machine**: Intel Core i5-1135G7, bare metal, cores held at the 2.4 GHz base frequency, turbo
> off, SMT siblings offline; kernel 7.0.0-34-generic; GCC 15.2.0, Release (`-O2`).

[`docs/RESULTS.md`](docs/RESULTS.md) has the full tables, the time orders wait in the queue at
full load, and the recording they come from.

---

## 3. Architecture

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

## 4. Scope and Limitations

- **Matching core only.** `TradeRingBuffer` stands in for a downstream consumer of trades: it
  keeps the most recent 4,096 and overwrites older ones. The figures cover matching and writing
  the trade record, not delivering trades.
- **Synthetic order flow.** `FeedSimulator` draws prices around a fixed base price and picks the
  target of a cancel at random, so many cancels find no resting order. The figures compare
  implementations under this workload, not behaviour on real market data.
- **Trusted input and fixed capacity.** The engine assumes that live order IDs are unique and
  quantities positive, and does not check them. Its memory pool is sized for the benchmarks; when
  it runs out, the producer waits for orders to come back.
- **One machine.** All figures come from one CPU model and describe that machine.

---

## 5. Build, Test and Reproduce

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

`ctest` runs the unit tests, the tests of the measurement scripts, the benchmarks' own consistency
checks and the TSC calibration; all pass in both configurations.

To reproduce the figures, the measurement scripts hold the machine in a measurement mode for the
length of a recording: frequency pinned at base with turbo off, SMT siblings offline, device
interrupts steered to other cores. Entering it needs root: install the helper once and add the
sudoers rule it prints. A recording takes about 105 minutes on the machine above.

```bash
sudo scripts/env/install_bench_env.sh          # once per machine, then add the printed rule
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
benchmarks/      Google Benchmark programs, with the earlier order book and pipeline as baselines
scripts/         Measurement tooling: env/ (measurement mode), record/, report/, diagnostics/
docs/            RESULTS.md, BENCHMARKING.md, AUDIT.md, and evidence/ with the raw recordings
```

`docs/AUDIT.md` reviews the project's first benchmark figures, measured on a cloud VM, and
explains why they were withdrawn.

---

## 7. Roadmap

The long-term goal is to drive the engine with real order and market data instead of the
synthetic feed, and to measure it on that flow.

---

## License

MIT. See [`LICENSE`](LICENSE).

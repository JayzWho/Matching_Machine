# Benchmark results

The full results of the current recording. The README's performance section
shows a selection of them.

All numbers come from one recording,
[`evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/`](evidence/11th-Gen-Intel-Core-i5-1135G7/2026-10-30_15-55-52_46cf493_control/),
of the code at commit `999c956` (recorded as `46cf493` before the history was
rewritten; see [`evidence/README.md`](evidence/README.md#commit-ids)). Its
`REPORT.md` gives the validity checks of every launch, and
[`BENCHMARKING.md`](BENCHMARKING.md) describes how a recording is made and
checked.

> **Machine**: Intel Core i5-1135G7, bare metal, cores held at the 2.4 GHz base frequency, turbo
> off, SMT siblings offline; kernel 7.0.0-34-generic; GCC 15.2.0, Release (`-O2`).
> **Method**: each benchmark program was launched 32 times, with 6 repetitions per benchmark and
> launch and the first left out as warm-up. Each value is the median of the 32 launch medians.

## 1. Order Book, Before and After the Optimizations (one thread)

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

## 2. Pipeline Throughput

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

## 3. Latency at 100K Orders

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
about 24% compared with section 2.

## 4. Queue Microbenchmarks

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

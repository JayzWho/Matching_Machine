/**
 * Pipeline benchmarks: the throughput of the SPSC pipeline, the single-threaded
 * baseline through each of the v3 book's two interfaces, and a run that prints the
 * queue-time and service-time percentiles in TSC ticks.
 *
 * Run through scripts/record/bench_record.py. When the process may use exactly two CPUs,
 * the producer is pinned to the lower-numbered and the consumer to the higher from their
 * creation; each benchmark's label records the placement.
 */

#include <benchmark/benchmark.h>
#include <cstring>
#include "matching_engine.h"
#include "order_book.h"
#include "feed_simulator.h"
#include "trade_ring_buffer.h"
#include "latency_recorder.h"
#include "thread_placement.h"

using namespace me;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Written into the label, so the raw JSON shows whether each launch pinned its threads.
static std::string placement(const ThreadCpus& cpus) {
    if (cpus.producer < 0) return "unpinned";
    return "producer=cpu" + std::to_string(cpus.producer) + " consumer=cpu" + std::to_string(cpus.consumer);
}

static Order make_order(uint64_t id, Side side, int64_t price, int64_t qty) {
    Order o{};
    o.order_id  = id;
    o.side      = side;
    o.price     = price;
    o.quantity  = qty;
    o.type      = OrderType::LIMIT;
    std::strncpy(o.symbol, "BTCUSD", 7);
    return o;
}

// ─── Pipeline throughput ─────────────────────────────────────────────────────
// Each iteration is one full start() → stop() run, thread start-up included.

static void BM_Pipeline_Throughput(benchmark::State& state) {
    const size_t order_count = static_cast<size_t>(state.range(0));
    constexpr double cancel_ratio = 0.2;
    const ThreadCpus cpus = allowed_cpu_pair();

    MatchingEngine engine;
    for (auto _ : state) {
        engine.start("BTCUSD", 100'000'000LL, order_count, cancel_ratio, cpus);
        engine.stop();
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(order_count)
    );
    state.SetLabel("orders=" + std::to_string(order_count) + " " + placement(cpus));
}

BENCHMARK(BM_Pipeline_Throughput)
    ->Arg(10'000)
    ->Arg(50'000)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3)
    ->UseRealTime();  // the main thread only waits in join, so CPU time says nothing

// ─── Single-threaded baseline: add_order_noalloc paired with add_order ───────
// One stream of FeedSimulator orders through either interface on the calling thread:
// add_order_noalloc, the pipeline consumer's interface, which also takes the cancels;
// or add_order plus cancel_order, which returns a vector of trades per order. Each
// iteration clears the book and resets the orders inside PauseTiming, and differing
// trade counts are reported as an error.

template <bool kNoalloc>
static void BM_SingleThread(benchmark::State& state) {
    constexpr size_t kBatchSize  = 10'000;
    constexpr double kCancelRatio = 0.2;

    FeedSimulator sim("BTCUSD", 100'000'000LL, /*cancel_ratio=*/kCancelRatio);
    auto orders = sim.generate_random(kBatchSize);
    // Stamped in advance, as in the pipeline, so the book never reads the TSC itself.
    for (auto& o : orders) o.timestamp_ns = 1;

    TradeRingBuffer<kTradeBufCap> trade_buf;
    OrderBook book("BTCUSD");

    int64_t first_trades = -1;
    bool same_every_iteration = true;
    for (auto _ : state) {
        state.PauseTiming();
        trade_buf.drain([](const Trade&) {});
        book.clear();
        for (auto& o : orders) {
            o.filled_qty = 0;
            o.status     = OrderStatus::NEW;
        }
        state.ResumeTiming();

        int64_t total_trades = 0;
        if constexpr (kNoalloc) {
            const size_t written = trade_buf.total_written();
            for (auto& o : orders) {
                book.add_order_noalloc(&o, trade_buf, [](Order*) {});
            }
            benchmark::DoNotOptimize(trade_buf.size());
            total_trades = static_cast<int64_t>(trade_buf.total_written() - written);
        } else {
            for (auto& o : orders) {
                if (o.type == OrderType::CANCEL) {
                    book.cancel_order(o.order_id);
                } else {
                    auto trades = book.add_order(&o);
                    total_trades += static_cast<int64_t>(trades.size());
                }
            }
        }

        if (first_trades < 0) first_trades = total_trades;
        same_every_iteration = same_every_iteration && total_trades == first_trades;
    }

    state.counters["trades_per_iteration"] = static_cast<double>(first_trades);
    if (!same_every_iteration) state.SkipWithError("iterations produced different numbers of trades");
    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(kBatchSize)
    );
    state.SetLabel(kNoalloc ? "single-thread noalloc" : "single-thread add_order");
}

BENCHMARK_TEMPLATE(BM_SingleThread, true)
    ->Name("BM_SingleThread_Baseline")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_SingleThread, false)
    ->Name("BM_SingleThread_Baseline_AddOrder")
    ->Unit(benchmark::kMicrosecond);

// ─── Latency report ──────────────────────────────────────────────────────────
// One run of 100K orders with latency recording, untimed; the percentiles go to stdout,
// where the recorder reads them.

static void BM_Pipeline_LatencyReport(benchmark::State& state) {
    const ThreadCpus cpus = allowed_cpu_pair();
    MatchingEngine engine;
    for (auto _ : state) {
        state.PauseTiming();
        engine.start("BTCUSD", 100'000'000LL, 100'000, 0.2, cpus, /*record_latency=*/true);
        engine.stop();
        state.ResumeTiming();

        std::printf("\n--- Pipeline Latency Report (100K orders) ---\n");
        engine.queue_latency().report("queue");
        engine.service_latency().report("service");
        std::printf("  (Trades produced: %zu)\n\n",
                    engine.trade_buffer().total_written());
    }
    state.SetLabel(placement(cpus));
}

BENCHMARK(BM_Pipeline_LatencyReport)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);

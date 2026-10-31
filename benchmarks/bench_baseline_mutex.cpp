/**
 * The mutex pipeline: the earlier system as a whole, kept as the baseline of the SPSC
 * pipeline. It differs from it in three ways at once: one std::queue guarded by a
 * std::mutex and a std::condition_variable, the v1 order book (order_book_v1.h), and one
 * make_shared allocation per order. It does not use the engine in src/.
 *
 * Thread placement, latency points and units are those of bench_matching_engine.cpp.
 */

#include <benchmark/benchmark.h>

#include "order.h"           // Order, Trade, Side, OrderType, OrderStatus
#include "feed_simulator.h"  // FeedSimulator::generate_random
#include "latency_recorder.h" // LatencyRecorder::now_serialized(), report()
#include "thread_placement.h" // ThreadCpus, PinnedThread, allowed_cpu_pair()
#include "order_book_v1.h"   // me_v1::OrderBookV1

#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <queue>
#include <memory>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdio>

// ─── MutexMatchingEngine ─────────────────────────────────────────────────────
// Deliberately plain. The producer allocates each order with make_shared, stamps it and
// pushes it under the lock, then notifies; the consumer waits, pops, matches with
// OrderBookV1 and records queue and service time like the SPSC pipeline.

class MutexMatchingEngine {
public:
    MutexMatchingEngine() = default;

    ~MutexMatchingEngine() {
        if (running_.load()) stop();
    }

    MutexMatchingEngine(const MutexMatchingEngine&) = delete;
    MutexMatchingEngine& operator=(const MutexMatchingEngine&) = delete;

    /// Same arguments as MatchingEngine::start.
    void start(std::string_view symbol,
               int64_t         base_price,
               size_t          order_count,
               double          cancel_ratio   = 0.2,
               me::ThreadCpus  cpus           = {},
               bool            record_latency = false) {
        if (running_.load()) throw std::runtime_error("already running");

        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            while (!shared_queue_.empty()) shared_queue_.pop();
        }
        producer_done_.store(false);
        queue_latency_.reset();
        service_latency_.reset();
        running_.store(true);

        try {
            consumer_thread_ = me::PinnedThread(cpus.consumer, [this, sym = std::string(symbol), record_latency] {
                consumer_loop(sym, record_latency);
            });

            producer_thread_ = me::PinnedThread(cpus.producer, [this, sym = std::string(symbol),
                                                                base_price, order_count, cancel_ratio,
                                                                record_latency] {
                producer_loop(sym, base_price, order_count, cancel_ratio, record_latency);
            });
        } catch (...) {
            // As in MatchingEngine: let a started consumer exit, then rethrow.
            {
                std::lock_guard<std::mutex> lk(queue_mtx_);
                producer_done_.store(true, std::memory_order_release);
            }
            cv_.notify_all();
            if (consumer_thread_.joinable()) consumer_thread_.join();
            running_.store(false);
            throw;
        }
    }

    void stop() {
        if (producer_thread_.joinable()) producer_thread_.join();
        if (consumer_thread_.joinable()) consumer_thread_.join();
        running_.store(false);
    }

    me::LatencyRecorder& queue_latency() { return queue_latency_; }
    me::LatencyRecorder& service_latency() { return service_latency_; }

private:
    void producer_loop(std::string_view symbol, int64_t base_price,
                       size_t order_count, double cancel_ratio, bool record_latency) {
        me::FeedSimulator sim(symbol, base_price, cancel_ratio);

        for (size_t i = 0; i < order_count; ++i) {
            auto order = std::make_shared<me::Order>();
            sim.generate_into(order.get());

            // Start of queue time, just before the push. Without latency recording an unfenced
            // read still stamps the order, as in the SPSC pipeline.
            order->timestamp_ns = record_latency ? me::LatencyRecorder::now_serialized()
                                                 : me::LatencyRecorder::now();

            {
                std::lock_guard<std::mutex> lk(queue_mtx_);
                shared_queue_.push(std::move(order));
            }
            cv_.notify_one();
        }

        // Set the flag under the lock: the consumer checks its condition holding the lock and
        // only then waits, and a flag set and notified outside the lock could fall between the
        // two steps and leave the consumer waiting forever.
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            producer_done_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }

    void consumer_loop(std::string_view symbol, bool record_latency) {
        me_v1::OrderBookV1 book(symbol);

        // The book holds raw pointers, so every order stays alive until the book is cleared.
        std::vector<std::shared_ptr<me::Order>> alive;

        while (true) {
            std::shared_ptr<me::Order> order;
            {
                std::unique_lock<std::mutex> lk(queue_mtx_);
                cv_.wait(lk, [this] {
                    return !shared_queue_.empty() ||
                           producer_done_.load(std::memory_order_acquire);
                });

                if (shared_queue_.empty()) {
                    break;
                }
                order = std::move(shared_queue_.front());
                shared_queue_.pop();
            }

            auto match = [&] {
                if (order->type == me::OrderType::CANCEL) {
                    book.cancel_order(order->order_id);
                } else {
                    book.add_order(order.get());
                }
            };

            if (record_latency) {
                const uint64_t t_pop = me::LatencyRecorder::now_serialized();
                const uint64_t t_push = order->timestamp_ns;

                match();

                const uint64_t t_done = me::LatencyRecorder::now_serialized();
                queue_latency_.record(t_pop - t_push);
                service_latency_.record(t_done - t_pop);
            } else {
                match();
            }

            alive.push_back(std::move(order));
        }

        book.clear();
    }

    std::queue<std::shared_ptr<me::Order>> shared_queue_;
    std::mutex               queue_mtx_;
    std::condition_variable  cv_;
    std::atomic<bool>        producer_done_{false};
    std::atomic<bool>        running_{false};

    me::PinnedThread producer_thread_;
    me::PinnedThread consumer_thread_;

    me::LatencyRecorder queue_latency_{100'000};
    me::LatencyRecorder service_latency_{100'000};
};

// ─── Throughput ──────────────────────────────────────────────────────────────

static std::string placement(const me::ThreadCpus& cpus) {
    if (cpus.producer < 0) return "unpinned";
    return "producer=cpu" + std::to_string(cpus.producer) + " consumer=cpu" + std::to_string(cpus.consumer);
}
// Mirrors BM_Pipeline_Throughput in bench_matching_engine.cpp.

static void BM_MutexPipeline_Throughput(benchmark::State& state) {
    const size_t order_count = static_cast<size_t>(state.range(0));
    constexpr double cancel_ratio = 0.2;
    const me::ThreadCpus cpus = me::allowed_cpu_pair();

    // Start the engine on a cache line. Otherwise stack randomization puts it at a
    // different offset in each launch, and throughput jumps between two levels with how
    // the fields both threads use fall on cache lines.
    alignas(64) MutexMatchingEngine engine;
    for (auto _ : state) {
        engine.start("BTCUSD", 100'000'000LL, order_count, cancel_ratio, cpus);
        engine.stop();
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(order_count)
    );
    state.SetLabel("orders=" + std::to_string(order_count) + " " + placement(cpus));
}

BENCHMARK(BM_MutexPipeline_Throughput)
    ->Arg(10'000)
    ->Arg(50'000)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3)
    ->UseRealTime();  // the main thread only waits in join

// ─── Latency report ──────────────────────────────────────────────────────────
// Mirrors BM_Pipeline_LatencyReport.

static void BM_MutexPipeline_LatencyReport(benchmark::State& state) {
    const me::ThreadCpus cpus = me::allowed_cpu_pair();
    alignas(64) MutexMatchingEngine engine;  // as in the throughput benchmark
    for (auto _ : state) {
        state.PauseTiming();
        engine.start("BTCUSD", 100'000'000LL, 100'000, 0.2, cpus, /*record_latency=*/true);
        engine.stop();
        state.ResumeTiming();

        std::printf("\n--- Mutex Baseline Latency Report (100K orders) ---\n");
        engine.queue_latency().report("queue");
        engine.service_latency().report("service");
        std::printf("\n");
    }
    state.SetLabel(placement(cpus));
}

BENCHMARK(BM_MutexPipeline_LatencyReport)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);

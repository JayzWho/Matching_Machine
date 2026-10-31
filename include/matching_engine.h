#pragma once

#include "order_book.h"
#include "order.h"
#include "memory_pool.h"
#include "spsc_ring_buffer.h"
#include "trade_ring_buffer.h"
#include "feed_simulator.h"
#include "latency_recorder.h"
#include "thread_placement.h"

#include <vector>
#include <string_view>
#include <string>
#include <unordered_map>
#include <memory>
#include <thread>
#include <atomic>
#include <cstddef>

namespace me {

// Queue capacities must be powers of two.
inline constexpr size_t kOrderQueueCap  = 4096;
inline constexpr size_t kReturnQueueCap = 4096;

/// Default pool size. In the worst case every order of a run rests, so a run of N orders
/// needs N slots; 128K covers the 100K-order benchmarks. When the pool runs out, the
/// producer waits for orders to come back.
inline constexpr size_t kOrderPoolCap   = 131072;

/**
 * Matching engine with two modes.
 *
 * Single-threaded: add_symbol() and submit() drive one order book per symbol directly.
 *
 * Pipeline: start() runs a producer thread that generates orders into pool slots and
 * pushes them through order_queue_, and a consumer thread that matches them with
 * add_order_noalloc() and sends every finished order back through return_queue_.
 * Only the producer touches the pool, so allocation needs no lock; the two SPSC queues
 * transfer ownership of each slot with release/acquire. Only the consumer writes
 * trade_buf_.
 */
class MatchingEngine {
public:
    explicit MatchingEngine(size_t pool_capacity = kOrderPoolCap) : order_pool_(pool_capacity) {}
    ~MatchingEngine();

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // ── Single-threaded mode ─────────────────────────────────────────────────

    void add_symbol(std::string_view symbol, int64_t base_price);

    /// Accepts limit orders only; send cancels to get_order_book(symbol)->cancel_order().
    /// Creates the symbol's book on first use.
    std::vector<Trade> submit(Order* order);

    OrderBook* get_order_book(std::string_view symbol);

    [[nodiscard]] size_t total_trades() const noexcept { return total_trades_; }

    // ── Pipeline mode ────────────────────────────────────────────────────────

    /**
     * Starts the producer and consumer threads. The producer generates `order_count`
     * orders and stops; the consumer drains the queue and stops after it. Each run starts
     * with a full pool and a fresh order book.
     *
     * @param cpus           CPUs to pin the producer and consumer to from their creation;
     *                       -1 leaves a thread to the scheduler.
     * @param record_latency Records queue and service time per order. The fenced TSC
     *                       reads slow the consumer, so leave it off when measuring
     *                       throughput.
     */
    void start(std::string_view symbol,
               int64_t         base_price,
               size_t          order_count    = 100'000,
               double          cancel_ratio   = 0.2,
               ThreadCpus      cpus           = {},
               bool            record_latency = false);

    /// Waits for the run to finish: joins the producer, then the consumer. It does not
    /// interrupt a run.
    void stop();

    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t consumed_count() const noexcept {
        return consumed_count_.load(std::memory_order_relaxed);
    }

    /// Read after stop(): a run that returned every order leaves the pool full.
    [[nodiscard]] size_t pool_available() const noexcept {
        return order_pool_.available();
    }

    /// From the producer's timestamp to the consumer's pop, in TSC ticks. Filled only with
    /// record_latency; read after stop().
    [[nodiscard]] LatencyRecorder& queue_latency() noexcept {
        return queue_latency_;
    }

    /// From the consumer's pop to the end of matching, in TSC ticks. As above.
    [[nodiscard]] LatencyRecorder& service_latency() noexcept {
        return service_latency_;
    }

    /// Holds only the most recent trades of a run; drain it after stop().
    [[nodiscard]] TradeRingBuffer<kTradeBufCap>& trade_buffer() noexcept {
        return trade_buf_;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
    size_t total_trades_ = 0;

    void producer_loop(std::string_view symbol,
                       int64_t         base_price,
                       size_t          order_count,
                       double          cancel_ratio,
                       bool            record_latency);

    void consumer_loop(std::string_view symbol, bool record_latency);

    // Producer thread only.
    MemoryPool<Order> order_pool_;

    SPSCRingBuffer<Order*, kOrderQueueCap>  order_queue_;

    // Consumer → producer: finished orders on their way back to the pool.
    SPSCRingBuffer<Order*, kReturnQueueCap> return_queue_;

    TradeRingBuffer<kTradeBufCap> trade_buf_;

    // Written by the consumer.
    LatencyRecorder queue_latency_{200'000};
    LatencyRecorder service_latency_{200'000};

    PinnedThread producer_thread_;
    PinnedThread consumer_thread_;

    std::atomic<bool> running_{false};

    // Set when the producer has pushed every order; the consumer exits once it also
    // finds order_queue_ empty.
    std::atomic<bool> producer_done_{false};

    // Set when the consumer exits; the producer then drains return_queue_ once more and exits.
    std::atomic<bool> consumer_done_{false};

    std::atomic<size_t> consumed_count_{0};
};

} // namespace me

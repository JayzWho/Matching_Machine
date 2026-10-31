#include "matching_engine.h"
#include <stdexcept>
#include <cstring>

namespace me {

MatchingEngine::~MatchingEngine() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

// ── Single-threaded mode ─────────────────────────────────────────────────────

void MatchingEngine::add_symbol(std::string_view symbol, int64_t /*base_price*/) {
    std::string key(symbol);
    if (books_.count(key)) return;
    books_[key] = std::make_unique<OrderBook>(symbol);
}

std::vector<Trade> MatchingEngine::submit(Order* order) {
    std::string key(order->symbol, strnlen(order->symbol, 8));
    auto it = books_.find(key);
    if (it == books_.end()) {
        books_[key] = std::make_unique<OrderBook>(key);
        it = books_.find(key);
    }

    auto trades = it->second->add_order(order);
    total_trades_ += trades.size();
    return trades;
}

OrderBook* MatchingEngine::get_order_book(std::string_view symbol) {
    std::string key(symbol);
    auto it = books_.find(key);
    return (it != books_.end()) ? it->second.get() : nullptr;
}

// ── Pipeline: start / stop ───────────────────────────────────────────────────

void MatchingEngine::start(std::string_view symbol,
                           int64_t         base_price,
                           size_t          order_count,
                           double          cancel_ratio,
                           ThreadCpus      cpus,
                           bool            record_latency) {
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("MatchingEngine: already running");
    }

    // Refill the pool: orders still resting at the end of the last run died with the
    // consumer's order book and never came back. Both threads are joined, so no slot is
    // in use.
    order_pool_.reset();
    producer_done_.store(false, std::memory_order_relaxed);
    consumer_done_.store(false, std::memory_order_relaxed);
    consumed_count_.store(0, std::memory_order_relaxed);
    queue_latency_.reset();
    service_latency_.reset();
    trade_buf_.drain([](const Trade&) {});
    running_.store(true, std::memory_order_release);

    try {
        consumer_thread_ = PinnedThread(cpus.consumer, [this, sym = std::string(symbol), record_latency] {
            consumer_loop(sym, record_latency);
        });

        producer_thread_ = PinnedThread(cpus.producer, [this, sym = std::string(symbol),
                                                        base_price, order_count, cancel_ratio, record_latency] {
            producer_loop(sym, base_price, order_count, cancel_ratio, record_latency);
        });
    } catch (...) {
        // A thread failed to start, e.g. on an unavailable CPU: let the consumer drain and
        // exit, then rethrow.
        producer_done_.store(true, std::memory_order_release);
        if (consumer_thread_.joinable()) consumer_thread_.join();
        running_.store(false, std::memory_order_release);
        throw;
    }
}

void MatchingEngine::stop() {
    if (producer_thread_.joinable()) {
        producer_thread_.join();
    }
    if (consumer_thread_.joinable()) {
        consumer_thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

// ── Producer thread ──────────────────────────────────────────────────────────

void MatchingEngine::producer_loop(std::string_view symbol,
                                   int64_t         base_price,
                                   size_t          order_count,
                                   double          cancel_ratio,
                                   bool            record_latency) {
    FeedSimulator sim(symbol, base_price, cancel_ratio);

    size_t pushed = 0;

    while (pushed < order_count) {
        // Spin for a free slot, reclaiming returned orders meanwhile.
        Order* slot = nullptr;
        while (true) {
            Order* ret = nullptr;
            while (return_queue_.try_pop(ret)) {
                order_pool_.deallocate(ret);
            }
            slot = order_pool_.allocate();
            if (slot != nullptr) break;
        }

        sim.generate_into(slot);
        // Start of queue time. Without latency recording an unfenced read still stamps the
        // order, so the book does not read the TSC again on the consumer thread.
        slot->timestamp_ns = record_latency ? LatencyRecorder::now_serialized() : LatencyRecorder::now();

        while (!order_queue_.try_push(slot)) {
            Order* ret = nullptr;
            while (return_queue_.try_pop(ret)) {
                order_pool_.deallocate(ret);
            }
        }

        ++pushed;
    }

    producer_done_.store(true, std::memory_order_release);

    // Keep draining return_queue_ until the consumer has exited, so that its pushes
    // never spin on a full return queue.
    while (true) {
        {
            Order* ret = nullptr;
            while (return_queue_.try_pop(ret)) {
                order_pool_.deallocate(ret);
            }
        }

        if (consumer_done_.load(std::memory_order_acquire)) {
            // The consumer's last pushes may have landed after the drain above.
            Order* ret = nullptr;
            while (return_queue_.try_pop(ret)) {
                order_pool_.deallocate(ret);
            }
            break;
        }

        std::this_thread::yield();
    }
}

// ── Consumer thread ──────────────────────────────────────────────────────────

void MatchingEngine::consumer_loop(std::string_view symbol, bool record_latency) {
    OrderBook book(symbol);

    // Sends a finished order back to the producer. A lambda, so the book's template
    // inlines it.
    auto deallocator = [this](Order* o) {
        while (!return_queue_.try_push(o)) {
        }
    };

    auto consume = [&](Order* order) {
        if (record_latency) {
            const uint64_t t_pop = LatencyRecorder::now_serialized();
            // Read the timestamp first: matching may return the order and the producer reuse it.
            const uint64_t t_push = order->timestamp_ns;

            book.add_order_noalloc(order, trade_buf_, deallocator);

            const uint64_t t_done = LatencyRecorder::now_serialized();
            queue_latency_.record(t_pop - t_push);
            service_latency_.record(t_done - t_pop);
        } else {
            book.add_order_noalloc(order, trade_buf_, deallocator);
        }

        consumed_count_.fetch_add(1, std::memory_order_relaxed);
    };

    while (true) {
        Order* order = nullptr;

        if (order_queue_.try_pop(order)) {
            consume(order);

        } else {
            if (producer_done_.load(std::memory_order_acquire)) {
                // Pop once more: the producer may have pushed its last order just before
                // setting producer_done_.
                if (!order_queue_.try_pop(order)) {
                    consumer_done_.store(true, std::memory_order_release);
                    break;
                }
                consume(order);
            }
        }
    }
}

} // namespace me

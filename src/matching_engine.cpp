#include "matching_engine.h"
#include <stdexcept>
#include <cstring>

namespace me {

// ── 析构函数：确保线程被 join ─────────────────────────────────────────────────

MatchingEngine::~MatchingEngine() {
    // 如果流水线还在运行，强制停止
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

// ── 单线程接口（向后兼容） ───────────────────────────────────────────────────

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

// ── 多线程流水线：start / stop ──────────────────────────────────────────────

void MatchingEngine::start(std::string_view symbol,
                           int64_t         base_price,
                           size_t          order_count,
                           double          cancel_ratio) {
    if (running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("MatchingEngine: already running");
    }

    // 重置状态
    producer_done_.store(false, std::memory_order_relaxed);
    consumer_done_.store(false, std::memory_order_relaxed);
    consumed_count_.store(0, std::memory_order_relaxed);
    latency_recorder_.reset();
    running_.store(true, std::memory_order_release);

    // 启动两个线程（先消费者，再生产者，避免生产者推入时消费者未就绪）
    consumer_thread_ = std::thread([this, sym = std::string(symbol)] {
        consumer_loop(sym);
    });

    producer_thread_ = std::thread([this, sym = std::string(symbol),
                                    base_price, order_count, cancel_ratio] {
        producer_loop(sym, base_price, order_count, cancel_ratio);
    });
}

void MatchingEngine::stop() {
    // 等待生产者线程完成
    if (producer_thread_.joinable()) {
        producer_thread_.join();
    }
    // 等待消费者线程完成（它会在 producer_done_ 且队列空时自行退出）
    if (consumer_thread_.joinable()) {
        consumer_thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

// ── 生产者线程主循环 ─────────────────────────────────────────────────────────

void MatchingEngine::producer_loop(std::string_view symbol,
                                   int64_t         base_price,
                                   size_t          order_count,
                                   double          cancel_ratio) {
    FeedSimulator sim(symbol, base_price);
    // 分批生成订单（避免一次性在堆上分配过多 vector<Order>）
    constexpr size_t kBatchSize = 256;

    size_t pushed = 0;

    while (pushed < order_count) {
        // ① 先从归还队列回收已完成的 Order*，归还内存池
        //    这必须在 allocate 之前，保证池有空闲槽位
        {
            Order* ret = nullptr;
            while (return_queue_.try_pop(ret)) {
                order_pool_.deallocate(ret);
            }
        }

        // ② 生成一批原始订单数据（使用 FeedSimulator，值语义）
        const size_t remaining   = order_count - pushed;
        const size_t batch_count = std::min(kBatchSize, remaining);
        auto raw_orders = sim.generate_random(batch_count, cancel_ratio);

        // ③ 从内存池分配 Order 对象，填充字段，推入 order_queue_
        for (const auto& raw : raw_orders) {
            // 自旋等待：内存池有空闲槽 + 队列有空位
            Order* slot = nullptr;
            while (true) {
                // 先尝试回收归还队列（防止池满时死锁）
                Order* ret = nullptr;
                while (return_queue_.try_pop(ret)) {
                    order_pool_.deallocate(ret);
                }

                slot = order_pool_.allocate();
                if (slot != nullptr) break;
                // 池满：自旋等待消费者归还
            }

            // 将原始数据复制到内存池槽位
            *slot = raw;
            // 生产者写入 timestamp_ns（端到端延迟起点）
            slot->timestamp_ns = LatencyRecorder::now();

            // 自旋直到 order_queue_ 有空位
            while (!order_queue_.try_push(slot)) {
                // 队列满时继续回收归还队列，避免生产者饥饿
                Order* ret = nullptr;
                while (return_queue_.try_pop(ret)) {
                    order_pool_.deallocate(ret);
                }
            }

            ++pushed;
        }
    }

    // ── 阶段二：生产任务结束，通知消费者，进入纯回收模式 ──────────────────────
    producer_done_.store(true, std::memory_order_release);

    // 持续 drain return_queue_，直到消费者完成并确认归还队列已清空
    // 这样可以保证消费者的 deallocate_cb_ 永远不会在 return_queue_ 满时死等
    while (true) {
        // drain return_queue_
        {
            Order* ret = nullptr;
            while (return_queue_.try_pop(ret)) {
                order_pool_.deallocate(ret);
            }
        }

        if (consumer_done_.load(std::memory_order_acquire)) {
            // 消费者已退出，最后再彻底排空一次（消费者退出前最后几次 push 可能刚到）
            Order* ret = nullptr;
            while (return_queue_.try_pop(ret)) {
                order_pool_.deallocate(ret);
            }
            break;
        }

        // 消费者还在处理，让出 CPU 避免空转（VM 2 核上非常重要）
        std::this_thread::yield();
    }
}

// ── 消费者线程主循环 ─────────────────────────────────────────────────────────

void MatchingEngine::consumer_loop(std::string_view symbol) {
    // 初始化消费者侧的 OrderBook
    OrderBook book(symbol);

    // 注册归还回调：挂单方被完全成交后，将其推入 return_queue_
    // 注意：return_queue_ 的 producer 是消费者线程（反向通道）
    book.set_deallocate_cb([this](Order* o) {
        // 自旋等待归还队列有空位（极低概率满）
        while (!return_queue_.try_push(o)) {
            // 如果这里阻塞，说明生产者回收过慢（扩大 kReturnQueueCap 可缓解）
        }
    });

    while (true) {
        Order* order = nullptr;

        if (order_queue_.try_pop(order)) {
            const uint64_t t_start = order->timestamp_ns;

            book.add_order_noalloc(order, trade_buf_);

            const uint64_t latency = LatencyRecorder::now() - t_start;
            latency_recorder_.record(latency);

            // 归还由 add_order_noalloc 内部的 deallocate_cb_ 统一负责：
            //   FILLED incoming 和 CANCEL 均在那里归还；挂单留在 PriceLevel 中

            consumed_count_.fetch_add(1, std::memory_order_relaxed);

        } else {
            // 队列为空：检查生产者是否已完成
            if (producer_done_.load(std::memory_order_acquire)) {
                // 再做一次 try_pop，避免 TOCTOU（生产者在设 done 前最后推入的订单）
                if (!order_queue_.try_pop(order)) {
                    // 真正排空：先设 consumer_done_，再退出
                    // 此处所有 add_order_noalloc 和 deallocate_cb_ 均已完成
                    consumer_done_.store(true, std::memory_order_release);
                    break;
                }
                // 处理最后一条
                const uint64_t t_start = order->timestamp_ns;
                book.add_order_noalloc(order, trade_buf_);
                const uint64_t latency = LatencyRecorder::now() - t_start;
                latency_recorder_.record(latency);
                // 归还同样由 add_order_noalloc 内部统一处理
                consumed_count_.fetch_add(1, std::memory_order_relaxed);
            }
            // 队列暂时为空但生产者未完成：继续自旋
        }
    }
}

} // namespace me

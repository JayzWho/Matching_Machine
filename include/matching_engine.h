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

// ─── 流水线容量常量 ──────────────────────────────────────────────────────────

/// 订单工作队列容量（Order* 指针，2 的幂）
inline constexpr size_t kOrderQueueCap  = 4096;

/// 归还队列容量（Consumer → Producer 归还 Order*，2 的幂）
inline constexpr size_t kReturnQueueCap = 4096;

/// 内存池默认容量（Order 对象槽位数）
/// 最坏情况：order_count 笔订单全部挂单无成交，需 = order_count。
/// 128K 可覆盖 10 万笔测试，并留有余量应对极端挂单堆积场景。
/// MemoryPool 现已改为堆分配（vector），此常量是构造函数未指定容量时的默认值。
inline constexpr size_t kOrderPoolCap   = 131072;

/**
 * @brief 撮合引擎
 *
 * 双模式设计：
 *
 * ① 单线程模式（向后兼容）：
 *   使用 add_symbol() / submit() 接口，与 Week 1-6 代码完全一致，
 *   适用于单元测试、单线程 benchmark、调试场景。
 *
 * ② 多线程 SPSC 流水线模式（Week 7-8 新增）：
 *   调用 start() 启动双线程，stop() 停止并 join。
 *
 *   架构：
 *     生产者线程:
 *       FeedSimulator → OrderMemoryPool::allocate()
 *       → SPSCRingBuffer<Order*> (order_queue_) → [消费者]
 *       + 定期从 return_queue_ 回收已完成的 Order*，归还内存池
 *
 *     消费者线程:
 *       [order_queue_] → OrderBook::add_order_noalloc()
 *       → TradeRingBuffer（成交写入）
 *       → 完成后将 Order* 推入 return_queue_（归还给生产者）
 *
 *   内存安全保证：
 *     - OrderMemoryPool 只由生产者线程调用（allocate + deallocate），无竞争
 *     - return_queue_ 是 Consumer→Producer 方向的 SPSC 队列，
 *       消费者是 producer，生产者是 consumer（方向与 order_queue_ 相反）
 *     - TradeRingBuffer 消费者线程独占写，无需同步
 */
class MatchingEngine {
public:
    /// @param pool_capacity 内存池的 Order 槽位数（测试可用小容量）
    explicit MatchingEngine(size_t pool_capacity = kOrderPoolCap) : order_pool_(pool_capacity) {}
    ~MatchingEngine();

    // 禁止拷贝（含线程和原子变量）
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // ── 单线程接口（向后兼容） ────────────────────────────────────────────────

    /// 注册一个品种的订单簿
    void add_symbol(std::string_view symbol, int64_t base_price);

    /// 提交订单（单线程版本）
    std::vector<Trade> submit(Order* order);

    /// 获取指定品种的订单簿（用于测试查询）
    OrderBook* get_order_book(std::string_view symbol);

    /// 总成交笔数（监控指标）
    [[nodiscard]] size_t total_trades() const noexcept { return total_trades_; }

    // ── 多线程流水线接口 ──────────────────────────────────────────────────────

    /**
     * @brief 启动 SPSC 流水线
     *
     * 注册品种、初始化 OrderBook、启动生产者和消费者两个线程。
     * 生产者持续从 FeedSimulator 生成 order_count 笔订单后退出，
     * 消费者持续处理直到生产者停止且队列排空。
     *
     * @param symbol       品种代码
     * @param base_price   基准价格（整数定价）
     * @param order_count  本次运行的订单总量（0 表示无限，直到 stop() 被调用）
     * @param cancel_ratio 撤单比例 [0.0, 1.0]，传给 FeedSimulator
     * @param cpus         生产者、消费者线程各自钉在哪个 CPU（创建时即生效）；
     *                     默认 {-1, -1}：不钉核，由调度器摆放
     * @param record_latency 记录每笔订单的排队时间和服务时间（queue_latency()、
     *                     service_latency()）。时间戳前后带栅栏，消费者每笔订单多花
     *                     几十纳秒，所以默认关闭，测吞吐时保持关闭
     */
    void start(std::string_view symbol,
               int64_t         base_price,
               size_t          order_count    = 100'000,
               double          cancel_ratio   = 0.2,
               ThreadCpus      cpus           = {},
               bool            record_latency = false);

    /**
     * @brief 停止流水线，等待双线程退出
     *
     * 设置 running_ = false，等待生产者先完成剩余推送，
     * 消费者排空队列后退出，最后 join 两个线程。
     */
    void stop();

    /// 流水线是否正在运行
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// 消费者已处理的订单总数（原子读，近似值）
    [[nodiscard]] size_t consumed_count() const noexcept {
        return consumed_count_.load(std::memory_order_relaxed);
    }

    /// 内存池的空闲槽位数（只在 stop() 之后读；用于检查内存池的归还）
    [[nodiscard]] size_t pool_available() const noexcept {
        return order_pool_.available();
    }

    /// 排队时间：从生产者打时间戳到消费者取出订单（start() 时打开 record_latency 才有采样，stop() 后读取）
    [[nodiscard]] LatencyRecorder& queue_latency() noexcept {
        return queue_latency_;
    }

    /// 服务时间：从消费者取出订单到撮合完成（同上）
    [[nodiscard]] LatencyRecorder& service_latency() noexcept {
        return service_latency_;
    }

    /// 消费者侧的 TradeRingBuffer（stop() 后可 drain）
    [[nodiscard]] TradeRingBuffer<kTradeBufCap>& trade_buffer() noexcept {
        return trade_buf_;
    }

private:
    // ── 单线程路径的成员 ─────────────────────────────────────────────────────
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
    size_t total_trades_ = 0;

    // ── 多线程流水线的成员 ──────────────────────────────────────────────────

    /// 生产者线程主循环
    void producer_loop(std::string_view symbol,
                       int64_t         base_price,
                       size_t          order_count,
                       double          cancel_ratio,
                       bool            record_latency);

    /// 消费者线程主循环
    void consumer_loop(std::string_view symbol, bool record_latency);

    // 内存池（生产者线程独占 allocate + deallocate）
    // 容量 kOrderPoolCap（128K）：覆盖 10 万笔全挂单最坏情况
    // 使用堆分配（vector），不占用栈空间
    MemoryPool<Order> order_pool_;

    // 订单工作队列：Producer 写 → Consumer 读
    SPSCRingBuffer<Order*, kOrderQueueCap>  order_queue_;

    // 归还队列：Consumer 写 → Producer 读（方向相反）
    SPSCRingBuffer<Order*, kReturnQueueCap> return_queue_;

    // Trade 环形缓冲区（Consumer 线程独占写）
    TradeRingBuffer<kTradeBufCap> trade_buf_;

    // 排队时间、服务时间的记录器（Consumer 线程写，stop() 后 report）
    LatencyRecorder queue_latency_{200'000};
    LatencyRecorder service_latency_{200'000};

    PinnedThread producer_thread_;
    PinnedThread consumer_thread_;

    // running_：流水线是否在运行（start/stop 控制）
    std::atomic<bool> running_{false};

    // producer_done_：生产者完成所有推送后设为 true
    //   消费者见此标志且 order_queue_ 空时退出
    std::atomic<bool> producer_done_{false};

    // consumer_done_：消费者完成所有处理后设为 true
    //   生产者（阶段二）见此标志且 return_queue_ 空时退出
    std::atomic<bool> consumer_done_{false};

    // 消费者已处理订单计数
    std::atomic<size_t> consumed_count_{0};
};

} // namespace me

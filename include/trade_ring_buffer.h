#pragma once

#include "order.h"
#include <array>
#include <cstddef>

namespace me {

/**
 * @brief Trade 专用预分配环形缓冲区
 *
 * 设计目标：彻底消除 match() 每次返回 std::vector<Trade> 的堆分配热点。
 *
 * 核心特性：
 *   - 消费者线程独占写（single-writer），无需原子操作，写入只是一次数组赋值
 *   - 固定容量预分配（std::array），整个生命周期零堆分配
 *   - 环形覆盖写入：满时覆盖最旧的记录（本项目下游抽象隐去，覆盖语义可接受）
 *   - drain() 接口模拟下游批量消费（传入 callable，遍历并清空）
 *
 * 对比 SPSCRingBuffer：
 *   - SPSCRingBuffer 用于跨线程传递（有 head_/tail_ 原子变量）
 *   - TradeRingBuffer 用于单线程内积累成交记录（无原子，纯数组写）
 *   - 二者都是预分配、无堆分配，但使用场景不同
 *
 * @tparam Capacity  最大缓冲 Trade 数量（建议 2 的幂，如 4096）
 */
template<size_t Capacity>
class TradeRingBuffer {
    static_assert(Capacity > 0, "Capacity must be > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2 for efficient masking");

public:
    TradeRingBuffer() = default;

    // 禁止拷贝（含大型数组，拷贝无意义）
    TradeRingBuffer(const TradeRingBuffer&) = delete;
    TradeRingBuffer& operator=(const TradeRingBuffer&) = delete;

    /**
     * @brief 写入一条成交记录（消费者线程独占调用）
     *
     * 直接写入到环形数组当前位置，O(1)，无堆分配。
     * 缓冲区满时覆盖最旧的记录（环形覆盖语义）。
     *
     * @param t 成交记录（按值拷贝写入数组）
     */
    void push_trade(const Trade& t) noexcept {
        buffer_[write_pos_ & mask_] = t;
        write_pos_++;
        if (count_ < Capacity) {
            ++count_;
        }
        ++total_written_;
    }

    /**
     * @brief 批量消费所有已写入的 Trade（模拟下游处理）
     *
     * 按写入顺序依次调用 fn(const Trade&)，完成后清空缓冲区。
     * 仅在消费者线程调用（与 push_trade 同线程）。
     *
     * @tparam Fn  callable，签名：void(const Trade&)
     * @return     处理的 Trade 数量
     */
    template<typename Fn>
    size_t drain(Fn&& fn) noexcept(noexcept(fn(std::declval<const Trade&>()))) {
        if (count_ == 0) return 0;

        // 计算环形缓冲区的读起点
        // 若未溢出（count_ < Capacity）：起点 = 0
        // 若已溢出（count_ == Capacity）：起点 = write_pos_（最旧条目位置）
        const size_t start = (count_ < Capacity) ? 0 : write_pos_;
        const size_t n = count_;

        for (size_t i = 0; i < n; ++i) {
            fn(buffer_[(start + i) & mask_]);
        }

        // 清空
        count_    = 0;
        write_pos_ = 0;
        return n;
    }

    /**
     * @brief 获取第 i 条已写入的 Trade（按写入顺序，0 = 最旧）
     *
     * 仅在消费者线程调用，主要用于测试验证。
     * @pre i < size()
     */
    [[nodiscard]] const Trade& operator[](size_t i) const noexcept {
        const size_t start = (count_ < Capacity) ? 0 : write_pos_;
        return buffer_[(start + i) & mask_];
    }

    /// 当前已写入（未 drain）的 Trade 数量
    [[nodiscard]] size_t size() const noexcept { return count_; }

    /// 缓冲区是否为空
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    /// 最大容量
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    /// 累计写入的 Trade 总数（含已 drain 的，单调递增）
    [[nodiscard]] size_t total_written() const noexcept { return total_written_; }

private:
    static constexpr size_t mask_ = Capacity - 1;

    std::array<Trade, Capacity> buffer_{};
    size_t write_pos_     = 0;   ///< 下一次写入的位置（未 mask）
    size_t count_         = 0;   ///< 当前有效 Trade 数（最多 = Capacity）
    size_t total_written_ = 0;   ///< 累计写入总数（含已 drain 的）
};

} // namespace me

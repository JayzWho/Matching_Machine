#pragma once

#include "order.h"
#include <array>
#include <cstddef>

namespace me {

/**
 * Pre-allocated buffer of the most recent trades, written and read by one thread.
 *
 * It stands in for a downstream consumer that would store or publish every trade: when
 * full it overwrites the oldest record, and drain() hands the records it holds to a
 * callable.
 */
template<size_t Capacity>
class TradeRingBuffer {
    static_assert(Capacity > 0, "Capacity must be > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2 for efficient masking");

public:
    TradeRingBuffer() = default;

    TradeRingBuffer(const TradeRingBuffer&) = delete;
    TradeRingBuffer& operator=(const TradeRingBuffer&) = delete;

    void push_trade(const Trade& t) noexcept {
        buffer_[write_pos_ & mask_] = t;
        write_pos_++;
        if (count_ < Capacity) {
            ++count_;
        }
        ++total_written_;
    }

    /// Returns the next slot for the caller to fill in place. The slot counts as written
    /// at once.
    [[nodiscard]] Trade& alloc_slot() noexcept {
        Trade& slot = buffer_[write_pos_ & mask_];
        ++write_pos_;
        if (count_ < Capacity) ++count_;
        ++total_written_;
        return slot;
    }

    /// Calls fn(const Trade&) on each held trade, oldest first, then empties the buffer.
    /// Returns the number of trades passed.
    template<typename Fn>
    size_t drain(Fn&& fn) noexcept(noexcept(fn(std::declval<const Trade&>()))) {
        if (count_ == 0) return 0;

        // Once the buffer has wrapped, the oldest record is at write_pos_.
        const size_t start = (count_ < Capacity) ? 0 : write_pos_;
        const size_t n = count_;

        for (size_t i = 0; i < n; ++i) {
            fn(buffer_[(start + i) & mask_]);
        }

        count_    = 0;
        write_pos_ = 0;
        return n;
    }

    /// The i-th held trade, 0 being the oldest. Requires i < size().
    [[nodiscard]] const Trade& operator[](size_t i) const noexcept {
        const size_t start = (count_ < Capacity) ? 0 : write_pos_;
        return buffer_[(start + i) & mask_];
    }

    [[nodiscard]] size_t size() const noexcept { return count_; }

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    /// Every trade ever written, including those drained or overwritten.
    [[nodiscard]] size_t total_written() const noexcept { return total_written_; }

private:
    static constexpr size_t mask_ = Capacity - 1;

    std::array<Trade, Capacity> buffer_{};
    size_t write_pos_     = 0;   ///< Next write, before masking.
    size_t count_         = 0;   ///< Trades held, at most Capacity.
    size_t total_written_ = 0;
};

} // namespace me

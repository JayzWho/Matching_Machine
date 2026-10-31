#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <optional>

namespace me {

/**
 * Lock-free ring buffer for exactly one producer thread and one consumer thread.
 *
 * Each side stores its own index with release and reads the other side's with acquire,
 * so an element written before a push is visible after the matching pop, and a slot is
 * reused only after its pop. A side reads its own index relaxed, since only it writes it.
 * `head_` and `tail_` sit on separate cache lines so the two sides do not falsely share
 * one. One slot stays empty to tell a full ring from an empty one, so it holds
 * Capacity - 1 elements. Capacity is a power of two so that wrapping is a mask.
 */
template<typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    SPSCRingBuffer() = default;

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    /// Producer only. Returns false when the ring is full.
    bool try_push(const T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & mask_;

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[tail] = item;

        // Publishes buffer_[tail] to the consumer.
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool try_push(T&& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & mask_;

        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[tail] = std::move(item);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /// Consumer only. Returns false when the ring is empty.
    bool try_pop(T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        item = buffer_[head];

        // Hands the slot back to the producer only after it has been read.
        head_.store((head + 1) & mask_, std::memory_order_release);
        return true;
    }

    std::optional<T> try_pop() noexcept {
        T item{};
        if (try_pop(item)) return item;
        return std::nullopt;
    }

    /// A snapshot that may be stale by the time it returns; for tests and monitoring.
    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (tail - head) & mask_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity - 1;
    }

private:
    static constexpr size_t mask_ = Capacity - 1;

    // head_: next slot to pop, written by the consumer. tail_: next slot to push,
    // written by the producer.
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

    alignas(64) std::array<T, Capacity> buffer_{};
};

} // namespace me

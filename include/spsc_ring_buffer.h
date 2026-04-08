#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <optional>

namespace me {

/**
 * @brief Lock-Free Single Producer Single Consumer Ring Buffer
 *
 * 设计要点：
 *   1. 仅适用于单生产者 + 单消费者场景（SPSC）。
 *      多生产者/消费者需要更重的同步机制（MPMC），此处不需要。
 *
 *   2. head_（消费者读取位置）和 tail_（生产者写入位置）各自独占一个
 *      Cache Line（alignas(64)），消除 False Sharing。
 *
 *   3. 内存序（Memory Order）选择：
 *      - try_push: tail_ 用 release（确保写入数据对消费者可见）
 *      - try_pop:  head_ 用 release（确保消费者对生产者可见）
 *      - 读取对方的 index 时用 acquire（配对读到对方的 release 写）
 *      - 读取自身 index 时用 relaxed（只有自己写，无需同步）
 *
 *   4. Capacity 必须是 2 的幂次，以便用位运算替代取模：
 *      index % Capacity  →  index & (Capacity - 1)  （更快）
 *
 * @tparam T         元素类型
 * @tparam Capacity  容量，必须是 2 的幂（如 1024, 4096）
 */
template<typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    SPSCRingBuffer() = default;

    // 禁止拷贝和移动（含有 atomic，语义上无意义）
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    /**
     * @brief 生产者：尝试写入一个元素
     * @return true 写入成功，false 队列已满
     *
     * 只允许单一生产者线程调用。
     */
    bool try_push(const T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & mask_;

        // 检查队列是否已满（next_tail 追上了 head）
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // 队列满
        }

        buffer_[tail] = item;

        // release：确保上面的写入（buffer_[tail] = item）
        // 在 tail_ 更新对消费者可见之前，已完全写入内存
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /**
     * @brief 生产者：尝试写入（移动语义版本）
     */
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

    /**
     * @brief 消费者：尝试读取一个元素
     * @param[out] item 读取到的元素（成功时填充）
     * @return true 读取成功，false 队列为空
     *
     * 只允许单一消费者线程调用。
     */
    bool try_pop(T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);

        // 检查队列是否为空（head 追上了 tail）
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;  // 队列空
        }

        item = buffer_[head];

        // release：确保上面的读取（item = buffer_[head]）
        // 在 head_ 更新对生产者可见之前已完成
        head_.store((head + 1) & mask_, std::memory_order_release);
        return true;
    }

    /**
     * @brief 消费者：返回 optional 版本（更现代的接口）
     */
    std::optional<T> try_pop() noexcept {
        T item{};
        if (try_pop(item)) return item;
        return std::nullopt;
    }

    /**
     * @brief 当前队列中的元素数量（近似值，仅供参考）
     *
     * 注意：在并发场景下，此值在返回瞬间可能已过时。
     * 仅用于调试和监控，不应用于控制流判断。
     */
    [[nodiscard]] size_t size() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (tail - head) & mask_;
    }

    /**
     * @brief 队列是否为空（近似值）
     */
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    /**
     * @brief 队列最大容量（实际可用 = Capacity - 1）
     *
     * 为什么是 Capacity-1：我们用"一个槽位不使用"来区分空和满的状态。
     * 若 head == tail → 空；若 (tail+1)%Capacity == head → 满。
     * 因此最多存 Capacity-1 个元素。
     */
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity - 1;
    }

private:
    static constexpr size_t mask_ = Capacity - 1;

    // 生产者写入位置（tail_）和消费者读取位置（head_）
    // 各自占一个 Cache Line，避免 False Sharing
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

    // 数据存储区，与控制变量分离（独立 Cache Line）
    alignas(64) std::array<T, Capacity> buffer_{};
};

} // namespace me

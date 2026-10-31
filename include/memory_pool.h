#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <type_traits>

namespace me {

/**
 * Fixed-capacity object pool. The slots are allocated once, on the heap, at construction;
 * allocate() and deallocate() then pop and push an index free list in O(1) without
 * calling malloc. Not thread-safe. allocate() returns nullptr when every slot is in use.
 */
template<typename T>
class MemoryPool {
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default constructible");

    using StorageSlot = std::aligned_storage_t<sizeof(T), alignof(T)>;

    static constexpr size_t kNull = SIZE_MAX;  // end of the free list

public:
    explicit MemoryPool(size_t max_objects)
        : max_objects_(max_objects),
          storage_(max_objects),
          free_list_(max_objects)
    {
        assert(max_objects > 0 && "max_objects must be > 0");
        init_free_list();
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    MemoryPool(MemoryPool&&) = default;
    MemoryPool& operator=(MemoryPool&&) = default;

    /// Returns a value-initialized T, or nullptr when the pool is exhausted.
    T* allocate() noexcept {
        if (free_head_ == kNull) return nullptr;

        const size_t idx = free_head_;
        free_head_ = free_list_[idx];
        --available_;

        return new (&storage_[idx]) T{};
    }

    /// `ptr` must come from this pool's allocate().
    void deallocate(T* ptr) noexcept {
        if (!ptr) return;

        const size_t idx = static_cast<size_t>(
            reinterpret_cast<StorageSlot*>(ptr) - storage_.data()
        );

        assert(idx < max_objects_ && "Pointer does not belong to this pool");

        ptr->~T();

        free_list_[idx] = free_head_;
        free_head_ = idx;
        ++available_;
    }

    /// Makes every slot free again without touching memory. Objects still allocated are
    /// abandoned without their destructor running, which is safe only for trivially
    /// destructible T such as Order.
    void reset() noexcept {
        init_free_list();
    }

    [[nodiscard]] size_t available() const noexcept { return available_; }

    /// True when no slot is free.
    [[nodiscard]] bool full() const noexcept { return available_ == 0; }

    /// True when every slot is free.
    [[nodiscard]] bool empty() const noexcept { return available_ == max_objects_; }

    [[nodiscard]] size_t capacity() const noexcept { return max_objects_; }

private:
    void init_free_list() noexcept {
        for (size_t i = 0; i < max_objects_ - 1; ++i) {
            free_list_[i] = i + 1;
        }
        free_list_[max_objects_ - 1] = kNull;
        free_head_  = 0;
        available_  = max_objects_;
    }

    size_t max_objects_;
    std::vector<StorageSlot> storage_;
    std::vector<size_t>      free_list_;  // free_list_[i]: the free slot after slot i
    size_t free_head_ = 0;
    size_t available_ = 0;
};

} // namespace me

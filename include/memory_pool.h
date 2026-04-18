#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <type_traits>

namespace me {

/**
 * @brief 固定大小对象池（Object Pool / Memory Pool）
 *
 * 核心思想：
 *   在构造时一次性堆分配 max_objects 个 T 类型的存储槽位（slab）。
 *   运行时 allocate() 从预分配空间直接取出，不调用 malloc/new。
 *   deallocate() 将对象归还到空闲链表，供下次复用。
 *   reset() 重建空闲链表（不释放/重分配内存），可在 benchmark 中复用池对象。
 *
 * 性能收益：
 *   - malloc/new 在高频场景下延迟不可控（可能触发系统调用、内存整理）
 *   - 对象池的 allocate 通常只是一次指针操作，延迟稳定在纳秒级
 *   - 所有对象连续存储，改善 Cache 局部性
 *
 * 限制：
 *   - 非线程安全（单线程使用，与 SPSC 模型配套）
 *   - 容量固定为构造时传入的 max_objects，超出时 allocate() 返回 nullptr
 *   - 对象 T 必须可默认构造
 *
 * 注意：存储使用堆分配（std::vector），避免大容量时将 MatchingEngine 放在栈上爆栈。
 *   例：131072 个 Order（64B）= 8MB storage + 1MB free_list，作为类成员不占栈空间。
 *
 * @tparam T  对象类型
 */
template<typename T>
class MemoryPool {
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default constructible");

    // 使用 aligned_storage 保证内存对齐（T 可能有对齐要求）
    using StorageSlot = std::aligned_storage_t<sizeof(T), alignof(T)>;

    static constexpr size_t kNull = SIZE_MAX;  // 链表终止哨兵

public:
    explicit MemoryPool(size_t max_objects)
        : max_objects_(max_objects),
          storage_(max_objects),
          free_list_(max_objects)
    {
        assert(max_objects > 0 && "max_objects must be > 0");
        init_free_list();
    }

    // 禁止拷贝（包含原始内存块，拷贝语义不合理）
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // 允许移动
    MemoryPool(MemoryPool&&) = default;
    MemoryPool& operator=(MemoryPool&&) = default;

    /**
     * @brief 分配一个 T 对象的内存，并构造（placement new）
     * @return 指向新对象的指针，如果池已满则返回 nullptr
     */
    T* allocate() noexcept {
        if (free_head_ == kNull) return nullptr;  // 池已满

        const size_t idx = free_head_;
        free_head_ = free_list_[idx];
        --available_;

        // placement new：在预分配的内存槽上构造对象
        return new (&storage_[idx]) T{};
    }

    /**
     * @brief 归还一个对象到池中（析构 + 将槽位放回空闲链表）
     * @param ptr 由本池 allocate() 返回的指针，不能传入其他来源的指针
     */
    void deallocate(T* ptr) noexcept {
        if (!ptr) return;

        // 计算槽位索引
        const size_t idx = static_cast<size_t>(
            reinterpret_cast<StorageSlot*>(ptr) - storage_.data()
        );

        assert(idx < max_objects_ && "Pointer does not belong to this pool");

        // 显式析构（与 placement new 配对）
        ptr->~T();

        // 将槽位放回空闲链表头部
        free_list_[idx] = free_head_;
        free_head_ = idx;
        ++available_;
    }

    /**
     * @brief 重置内存池（不释放/重分配内存）
     *
     * 重建空闲链表，使所有槽位重新可用。
     * 注意：调用前必须确保所有已分配的对象均已析构（deallocate），
     *       否则会发生内存泄漏（对象析构函数不会被再次调用）。
     * 适用于 benchmark 场景：一次构造、多次复用，避免反复堆分配开销。
     */
    void reset() noexcept {
        init_free_list();
    }

    /// 当前可用槽位数
    [[nodiscard]] size_t available() const noexcept { return available_; }

    /// 是否已满
    [[nodiscard]] bool full() const noexcept { return available_ == 0; }

    /// 是否全部空闲
    [[nodiscard]] bool empty() const noexcept { return available_ == max_objects_; }

    /// 总容量（运行期值）
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
    std::vector<StorageSlot> storage_;    // 堆分配的预分配内存块
    std::vector<size_t>      free_list_;  // 堆分配的空闲链表（索引数组）
    size_t free_head_ = 0;
    size_t available_ = 0;
};

} // namespace me

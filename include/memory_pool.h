#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <type_traits>

namespace me {

/**
 * @brief 固定大小对象池（Object Pool / Memory Pool）
 *
 * 核心思想：
 *   在程序启动时一次性分配 MaxObjects 个 T 类型的存储槽位（slab）。
 *   运行时 allocate() 从预分配空间直接取出，不调用 malloc/new。
 *   deallocate() 将对象归还到空闲链表，供下次复用。
 *
 * 性能收益：
 *   - malloc/new 在高频场景下延迟不可控（可能触发系统调用、内存整理）
 *   - 对象池的 allocate 通常只是一次指针操作，延迟稳定在纳秒级
 *   - 所有对象连续存储，改善 Cache 局部性
 *
 * 限制：
 *   - 非线程安全（单线程使用，与 SPSC 模型配套）
 *   - 容量固定为 MaxObjects，超出时 allocate() 返回 nullptr
 *   - 对象 T 必须可默认构造
 *
 * @tparam T          对象类型
 * @tparam MaxObjects 最大对象数量
 */
template<typename T, size_t MaxObjects>
class MemoryPool {
    static_assert(MaxObjects > 0, "MaxObjects must be > 0");
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default constructible");

public:
    MemoryPool() {
        // 初始化空闲链表：所有槽位链接成一个单向链表
        // free_list_[i] 存储"下一个空闲槽的索引"
        for (size_t i = 0; i < MaxObjects - 1; ++i) {
            free_list_[i] = i + 1;
        }
        free_list_[MaxObjects - 1] = kNull;  // 链表末尾
        free_head_ = 0;                       // 头指针指向第一个空闲槽
        available_ = MaxObjects;
    }

    // 禁止拷贝（包含原始内存块，拷贝语义不合理）
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    /**
     * @brief 分配一个 T 对象的内存，并构造（placement new）
     * @return 指向新对象的指针，如果池已满则返回 nullptr
     *
     * 注意：对象被默认构造初始化。如果需要特定初始值，调用方负责赋值。
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

        assert(idx < MaxObjects && "Pointer does not belong to this pool");

        // 显式析构（与 placement new 配对）
        ptr->~T();

        // 将槽位放回空闲链表头部
        free_list_[idx] = free_head_;
        free_head_ = idx;
        ++available_;
    }

    /// 当前可用槽位数
    [[nodiscard]] size_t available() const noexcept { return available_; }

    /// 是否已满
    [[nodiscard]] bool full() const noexcept { return available_ == 0; }

    /// 是否全部空闲（池里没有任何对象被使用）
    [[nodiscard]] bool empty() const noexcept { return available_ == MaxObjects; }

    /// 总容量
    [[nodiscard]] static constexpr size_t capacity() noexcept { return MaxObjects; }

private:
    static constexpr size_t kNull = SIZE_MAX;  // 链表终止哨兵

    // 使用 aligned_storage 保证内存对齐（T 可能有对齐要求）
    using StorageSlot = std::aligned_storage_t<sizeof(T), alignof(T)>;

    std::array<StorageSlot, MaxObjects> storage_{};   // 预分配内存块
    std::array<size_t, MaxObjects>      free_list_{}; // 空闲链表（索引数组）
    size_t free_head_ = 0;    // 空闲链表头部索引
    size_t available_ = 0;    // 当前可用槽位数（构造函数中初始化）
};

} // namespace me

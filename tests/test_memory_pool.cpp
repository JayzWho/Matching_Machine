#include "memory_pool.h"
#include "order.h"
#include <gtest/gtest.h>

using namespace me;

// ── 基础功能测试 ──────────────────────────────────────────────────────────────

TEST(MemoryPoolTest, InitialState) {
    MemoryPool<int, 8> pool;
    EXPECT_EQ(pool.available(), 8u);
    EXPECT_EQ(pool.capacity(), 8u);
    EXPECT_TRUE(pool.empty());   // empty() = 全部空闲
    EXPECT_FALSE(pool.full());
}

TEST(MemoryPoolTest, AllocateAndDeallocate) {
    MemoryPool<int, 4> pool;

    int* p = pool.allocate();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 3u);

    *p = 42;
    EXPECT_EQ(*p, 42);

    pool.deallocate(p);
    EXPECT_EQ(pool.available(), 4u);
    EXPECT_TRUE(pool.empty());
}

TEST(MemoryPoolTest, AllocateFillsPool) {
    MemoryPool<int, 3> pool;

    int* p1 = pool.allocate();
    int* p2 = pool.allocate();
    int* p3 = pool.allocate();

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.available(), 0u);

    // 超出容量返回 nullptr
    int* p4 = pool.allocate();
    EXPECT_EQ(p4, nullptr);

    pool.deallocate(p1);
    pool.deallocate(p2);
    pool.deallocate(p3);
    EXPECT_TRUE(pool.empty());
}

TEST(MemoryPoolTest, ReuseAfterDeallocate) {
    MemoryPool<int, 2> pool;

    int* p1 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    pool.deallocate(p1);

    // 归还后可以再次分配
    int* p2 = pool.allocate();
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pool.available(), 1u);
    pool.deallocate(p2);
}

TEST(MemoryPoolTest, DeallocateNullptrIsSafe) {
    MemoryPool<int, 4> pool;
    // deallocate(nullptr) 不应崩溃
    pool.deallocate(nullptr);
    EXPECT_EQ(pool.available(), 4u);
}

TEST(MemoryPoolTest, MultipleAllocationsUnique) {
    // 所有分配出来的指针应该不同
    constexpr size_t N = 16;
    MemoryPool<int, N> pool;

    std::array<int*, N> ptrs{};
    for (size_t i = 0; i < N; ++i) {
        ptrs[i] = pool.allocate();
        ASSERT_NE(ptrs[i], nullptr);
    }

    // 验证所有指针唯一
    for (size_t i = 0; i < N; ++i) {
        for (size_t j = i + 1; j < N; ++j) {
            EXPECT_NE(ptrs[i], ptrs[j]) << "Duplicate pointer at " << i << " and " << j;
        }
    }

    for (size_t i = 0; i < N; ++i) pool.deallocate(ptrs[i]);
}

// ── 与 Order 结构体配合测试 ──────────────────────────────────────────────────

TEST(MemoryPoolTest, WorksWithOrderStruct) {
    MemoryPool<Order, 8> pool;

    Order* o = pool.allocate();
    ASSERT_NE(o, nullptr);

    o->order_id = 12345;
    o->price    = 100'000'000;
    o->quantity = 10;
    o->side     = Side::BUY;

    EXPECT_EQ(o->order_id, 12345u);
    EXPECT_EQ(o->remaining_qty(), 10);

    pool.deallocate(o);
    EXPECT_EQ(pool.available(), 8u);
}

TEST(MemoryPoolTest, AllocateFreeAllocateReusesMemory) {
    // 池容量为 1：先分配，归还，再分配，验证可以正常重用
    MemoryPool<Order, 1> pool;

    Order* p1 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    p1->order_id = 1;
    pool.deallocate(p1);

    Order* p2 = pool.allocate();
    ASSERT_NE(p2, nullptr);
    // 归还后对象应被默认构造，order_id 应为 0
    EXPECT_EQ(p2->order_id, 0u);
    pool.deallocate(p2);
}

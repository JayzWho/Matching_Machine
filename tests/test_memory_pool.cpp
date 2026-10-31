#include "memory_pool.h"
#include "order.h"
#include <gtest/gtest.h>
#include <array>

using namespace me;

// ── Basics ───────────────────────────────────────────────────────────────────

TEST(MemoryPoolTest, InitialState) {
    MemoryPool<int> pool(8);
    EXPECT_EQ(pool.available(), 8u);
    EXPECT_EQ(pool.capacity(), 8u);
    EXPECT_TRUE(pool.empty());  // empty(): every slot free
    EXPECT_FALSE(pool.full());
}

TEST(MemoryPoolTest, AllocateAndDeallocate) {
    MemoryPool<int> pool(4);

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
    MemoryPool<int> pool(3);

    int* p1 = pool.allocate();
    int* p2 = pool.allocate();
    int* p3 = pool.allocate();

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.available(), 0u);

    int* p4 = pool.allocate();
    EXPECT_EQ(p4, nullptr);

    pool.deallocate(p1);
    pool.deallocate(p2);
    pool.deallocate(p3);
    EXPECT_TRUE(pool.empty());
}

TEST(MemoryPoolTest, ReuseAfterDeallocate) {
    MemoryPool<int> pool(2);

    int* p1 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    pool.deallocate(p1);

    int* p2 = pool.allocate();
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pool.available(), 1u);
    pool.deallocate(p2);
}

TEST(MemoryPoolTest, DeallocateNullptrIsSafe) {
    MemoryPool<int> pool(4);
    pool.deallocate(nullptr);
    EXPECT_EQ(pool.available(), 4u);
}

TEST(MemoryPoolTest, MultipleAllocationsUnique) {
    constexpr size_t N = 16;
    MemoryPool<int> pool(N);

    std::array<int*, N> ptrs{};
    for (size_t i = 0; i < N; ++i) {
        ptrs[i] = pool.allocate();
        ASSERT_NE(ptrs[i], nullptr);
    }

    for (size_t i = 0; i < N; ++i) {
        for (size_t j = i + 1; j < N; ++j) {
            EXPECT_NE(ptrs[i], ptrs[j]) << "Duplicate pointer at " << i << " and " << j;
        }
    }

    for (size_t i = 0; i < N; ++i) pool.deallocate(ptrs[i]);
}

// ── reset() ──────────────────────────────────────────────────────────────────

TEST(MemoryPoolTest, Reset_RestoresFullCapacity) {
    MemoryPool<int> pool(4);

    int* p1 = pool.allocate();
    int* p2 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pool.available(), 2u);

    pool.deallocate(p1);
    pool.deallocate(p2);
    pool.reset();

    EXPECT_EQ(pool.available(), 4u);
    EXPECT_TRUE(pool.empty());

    int* p3 = pool.allocate();
    ASSERT_NE(p3, nullptr);
    pool.deallocate(p3);
}

// ── With Order ───────────────────────────────────────────────────────────────

TEST(MemoryPoolTest, WorksWithOrderStruct) {
    MemoryPool<Order> pool(8);

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
    MemoryPool<Order> pool(1);

    Order* p1 = pool.allocate();
    ASSERT_NE(p1, nullptr);
    p1->order_id = 1;
    pool.deallocate(p1);

    Order* p2 = pool.allocate();
    ASSERT_NE(p2, nullptr);
    // A reused slot is value-initialized again.
    EXPECT_EQ(p2->order_id, 0u);
    pool.deallocate(p2);
}

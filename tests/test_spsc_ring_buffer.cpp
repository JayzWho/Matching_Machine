#include "spsc_ring_buffer.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <numeric>

using namespace me;

// ── One thread ───────────────────────────────────────────────────────────────

TEST(SPSCRingBufferTest, InitiallyEmpty) {
    SPSCRingBuffer<int, 8> rb;
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0u);
}

TEST(SPSCRingBufferTest, PushAndPop) {
    SPSCRingBuffer<int, 8> rb;
    EXPECT_TRUE(rb.try_push(42));
    EXPECT_FALSE(rb.empty());

    int val = 0;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_TRUE(rb.empty());
}

TEST(SPSCRingBufferTest, FIFOOrder) {
    SPSCRingBuffer<int, 16> rb;
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(rb.try_push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int val = -1;
        EXPECT_TRUE(rb.try_pop(val));
        EXPECT_EQ(val, i);
    }
}

TEST(SPSCRingBufferTest, FullReturnsFalse) {
    // Capacity 4 holds 3: one slot stays empty to tell full from empty.
    SPSCRingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.try_push(1));
    EXPECT_TRUE(rb.try_push(2));
    EXPECT_TRUE(rb.try_push(3));
    EXPECT_FALSE(rb.try_push(4));
    EXPECT_EQ(rb.size(), 3u);
}

TEST(SPSCRingBufferTest, EmptyReturnsFalse) {
    SPSCRingBuffer<int, 8> rb;
    int val = 0;
    EXPECT_FALSE(rb.try_pop(val));
}

TEST(SPSCRingBufferTest, OptionalInterface) {
    SPSCRingBuffer<int, 8> rb;
    rb.try_push(99);
    auto result = rb.try_pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 99);

    auto empty_result = rb.try_pop();
    EXPECT_FALSE(empty_result.has_value());
}

TEST(SPSCRingBufferTest, WrapAround) {
    // Fill, drain and fill again so that the indices wrap.
    SPSCRingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.try_push(1));
    EXPECT_TRUE(rb.try_push(2));
    EXPECT_TRUE(rb.try_push(3));

    int val = 0;
    EXPECT_TRUE(rb.try_pop(val)); EXPECT_EQ(val, 1);
    EXPECT_TRUE(rb.try_pop(val)); EXPECT_EQ(val, 2);

    EXPECT_TRUE(rb.try_push(4));
    EXPECT_TRUE(rb.try_push(5));

    EXPECT_TRUE(rb.try_pop(val)); EXPECT_EQ(val, 3);
    EXPECT_TRUE(rb.try_pop(val)); EXPECT_EQ(val, 4);
    EXPECT_TRUE(rb.try_pop(val)); EXPECT_EQ(val, 5);
    EXPECT_TRUE(rb.empty());
}

TEST(SPSCRingBufferTest, CapacityConstant) {
    EXPECT_EQ((SPSCRingBuffer<int, 8>::capacity()), 7u);    // Capacity-1
    EXPECT_EQ((SPSCRingBuffer<int, 16>::capacity()), 15u);
    EXPECT_EQ((SPSCRingBuffer<int, 1024>::capacity()), 1023u);
}

// ── Two threads ──────────────────────────────────────────────────────────────

TEST(SPSCRingBufferTest, ConcurrentProducerConsumer) {
    constexpr int kCount = 10000;
    SPSCRingBuffer<int, 1024> rb;

    std::vector<int> received;
    received.reserve(kCount);

    std::thread producer([&]() {
        for (int i = 0; i < kCount; ++i) {
            while (!rb.try_push(i)) {
            }
        }
    });

    std::thread consumer([&]() {
        int val = 0;
        int count = 0;
        while (count < kCount) {
            if (rb.try_pop(val)) {
                received.push_back(val);
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(received[i], i) << "Data corrupted at index " << i;
    }
}

TEST(SPSCRingBufferTest, ConcurrentSum) {
    constexpr int kCount = 1000;
    SPSCRingBuffer<int, 256> rb;

    int64_t total = 0;
    const int64_t expected = static_cast<int64_t>(kCount) * (kCount - 1) / 2;

    std::thread producer([&]() {
        for (int i = 0; i < kCount; ++i) {
            while (!rb.try_push(i)) {}
        }
    });

    std::thread consumer([&]() {
        int val = 0;
        int count = 0;
        while (count < kCount) {
            if (rb.try_pop(val)) {
                total += val;
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(total, expected);
}

/**
 * @file test_spsc_queue.cpp
 * @brief Unit tests for the SPSC Queue.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#include <gtest/gtest.h>
#include <core/SPSCQueue.hpp>

#include <thread>
#include <vector>
#include <atomic>

using namespace hft;

class SPSCQueueTest : public ::testing::Test {
protected:
    static constexpr std::size_t QUEUE_SIZE = 1024;
    SPSCQueue<int, QUEUE_SIZE> queue;
};

//==============================================================================
// Basic Operations
//==============================================================================

TEST_F(SPSCQueueTest, InitialStateIsEmpty) {
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.sizeApprox(), 0);
    EXPECT_FALSE(queue.full());
}

TEST_F(SPSCQueueTest, PushSingleElement) {
    EXPECT_TRUE(queue.tryPush(42));
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.sizeApprox(), 1);
}

TEST_F(SPSCQueueTest, PopSingleElement) {
    (void)queue.tryPush(42);

    int value = 0;
    EXPECT_TRUE(queue.tryPop(value));
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(queue.empty());
}

TEST_F(SPSCQueueTest, PopFromEmptyQueueFails) {
    int value = 0;
    EXPECT_FALSE(queue.tryPop(value));
}

TEST_F(SPSCQueueTest, PopOptional) {
    (void)queue.tryPush(42);

    auto result = queue.pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);

    result = queue.pop();
    EXPECT_FALSE(result.has_value());
}

TEST_F(SPSCQueueTest, FrontElement) {
    (void)queue.tryPush(42);

    const int* front = queue.front();
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(*front, 42);

    // Front doesn't remove
    front = queue.front();
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(*front, 42);
}

TEST_F(SPSCQueueTest, FrontOnEmptyQueueReturnsNull) {
    EXPECT_EQ(queue.front(), nullptr);
}

//==============================================================================
// Capacity Tests
//==============================================================================

TEST_F(SPSCQueueTest, Capacity) {
    // Capacity is QUEUE_SIZE - 1 (one slot reserved)
    EXPECT_EQ(queue.capacity(), QUEUE_SIZE - 1);
}

TEST_F(SPSCQueueTest, FillQueue) {
    for (std::size_t i = 0; i < queue.capacity(); ++i) {
        EXPECT_TRUE(queue.tryPush(static_cast<int>(i)));
    }

    EXPECT_TRUE(queue.full());
    EXPECT_FALSE(queue.tryPush(999));  // Queue full
}

TEST_F(SPSCQueueTest, FillAndDrainQueue) {
    // Fill
    for (std::size_t i = 0; i < queue.capacity(); ++i) {
        EXPECT_TRUE(queue.tryPush(static_cast<int>(i)));
    }

    // Drain
    for (std::size_t i = 0; i < queue.capacity(); ++i) {
        int value;
        EXPECT_TRUE(queue.tryPop(value));
        EXPECT_EQ(value, static_cast<int>(i));
    }

    EXPECT_TRUE(queue.empty());
}

//==============================================================================
// FIFO Order
//==============================================================================

TEST_F(SPSCQueueTest, FIFOOrder) {
    std::vector<int> input = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    for (int v : input) {
        EXPECT_TRUE(queue.tryPush(v));
    }

    for (int expected : input) {
        int value;
        EXPECT_TRUE(queue.tryPop(value));
        EXPECT_EQ(value, expected);
    }
}

//==============================================================================
// Emplace
//==============================================================================

TEST_F(SPSCQueueTest, Emplace) {
    EXPECT_TRUE(queue.tryEmplace(42));

    int value;
    EXPECT_TRUE(queue.tryPop(value));
    EXPECT_EQ(value, 42);
}

//==============================================================================
// Move Semantics
//==============================================================================

struct MoveOnlyType {
    int value;
    bool moved = false;

    MoveOnlyType() noexcept : value(0), moved(false) {}
    MoveOnlyType(int v) : value(v) {}
    MoveOnlyType(const MoveOnlyType&) = delete;
    MoveOnlyType& operator=(const MoveOnlyType&) = delete;
    MoveOnlyType(MoveOnlyType&& other) noexcept : value(other.value), moved(false) {
        other.moved = true;
    }
    MoveOnlyType& operator=(MoveOnlyType&& other) noexcept {
        value = other.value;
        other.moved = true;
        return *this;
    }
};

TEST(SPSCQueueMoveTest, MoveOnlyTypes) {
    SPSCQueue<MoveOnlyType, 64> queue;

    MoveOnlyType item(42);
    EXPECT_TRUE(queue.tryPush(std::move(item)));
    EXPECT_TRUE(item.moved);

    MoveOnlyType result(0);
    EXPECT_TRUE(queue.tryPop(result));
    EXPECT_EQ(result.value, 42);
}

//==============================================================================
// Multi-threaded Tests
//==============================================================================

TEST_F(SPSCQueueTest, SingleProducerSingleConsumer) {
    constexpr int NUM_ITEMS = 100000;
    std::atomic<bool> producerDone{false};

    std::thread producer([this, &producerDone]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!queue.tryPush(i)) {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([this, &producerDone]() {
        constexpr int NUM_ITEMS_LOCAL = NUM_ITEMS;  // Avoid capture
        int expected = 0;
        while (expected < NUM_ITEMS_LOCAL) {
            int value;
            if (queue.tryPop(value)) {
                EXPECT_EQ(value, expected);
                ++expected;
            } else if (producerDone.load(std::memory_order_acquire)) {
                // Producer done, but we might have remaining items
                if (queue.empty()) break;
            }
        }
        EXPECT_EQ(expected, NUM_ITEMS_LOCAL);
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(queue.empty());
}

TEST_F(SPSCQueueTest, ConcurrentAccessMaintainsFIFO) {
    constexpr int NUM_ITEMS = 10000;
    std::vector<int> consumed;
    consumed.reserve(NUM_ITEMS);
    std::atomic<bool> done{false};

    std::thread producer([this, &done]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!queue.tryPush(i)) {
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([this, &consumed, &done]() {
        while (true) {
            int value;
            if (queue.tryPop(value)) {
                consumed.push_back(value);
            } else if (done.load(std::memory_order_acquire) && queue.empty()) {
                break;
            }
        }
    });

    producer.join();
    consumer.join();

    // Verify FIFO order
    ASSERT_EQ(consumed.size(), NUM_ITEMS);
    for (int i = 0; i < NUM_ITEMS; ++i) {
        EXPECT_EQ(consumed[static_cast<std::size_t>(i)], i);
    }
}

//==============================================================================
// Wraparound
//==============================================================================

TEST_F(SPSCQueueTest, Wraparound) {
    // Fill partially, drain, repeat several times to test wraparound
    for (int round = 0; round < 10; ++round) {
        // Push half capacity
        for (std::size_t i = 0; i < queue.capacity() / 2; ++i) {
            EXPECT_TRUE(queue.tryPush(static_cast<int>(static_cast<std::size_t>(round) * 1000 + i)));
        }

        // Pop all
        int value;
        std::size_t count = 0;
        while (queue.tryPop(value)) {
            EXPECT_EQ(value, static_cast<int>(static_cast<std::size_t>(round) * 1000 + count));
            ++count;
        }

        EXPECT_EQ(count, queue.capacity() / 2);
    }
}

//==============================================================================
// Size Approximation
//==============================================================================

TEST_F(SPSCQueueTest, SizeApprox) {
    EXPECT_EQ(queue.sizeApprox(), 0);

    for (int i = 0; i < 100; ++i) {
        (void)queue.tryPush(i);
    }
    EXPECT_EQ(queue.sizeApprox(), 100);

    for (int i = 0; i < 50; ++i) {
        int v;
        (void)queue.tryPop(v);
    }
    EXPECT_EQ(queue.sizeApprox(), 50);
}

//==============================================================================
// Blocking Queue Tests
//==============================================================================

TEST(SPSCQueueBlockingTest, BlockingPush) {
    SPSCQueueBlocking<int, 64> queue;

    std::thread producer([&queue]() {
        for (int i = 0; i < 1000; ++i) {
            queue.push(i);
        }
    });

    std::thread consumer([&queue]() {
        for (int i = 0; i < 1000; ++i) {
            int value = queue.popWait();
            EXPECT_EQ(value, i);
        }
    });

    producer.join();
    consumer.join();
}

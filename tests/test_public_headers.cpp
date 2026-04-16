/**
 * @file test_public_headers.cpp
 * @brief Compile smoke tests for public headers.
 */

#include <core/MemoryArena.hpp>
#include <core/SPSCQueue.hpp>
#include <core/Timestamp.hpp>
#include <core/Types.hpp>
#include <gateway/OrderEntryGateway.hpp>
#include <market_data/MarketDataHandler.hpp>
#include <market_data/QuickFixApplication.hpp>
#include <orderbook/OrderBook.hpp>
#include <strategy/StrategyEngine.hpp>
#include <utils/Logger.hpp>

#include <gtest/gtest.h>

#include <memory>

using namespace hft;

TEST(PublicHeadersTest, HeadersCompileAndCoreTypesInstantiate) {
    // Default aliases own large fixed-capacity buffers; keep them off the test
    // thread stack for Windows CI's smaller default stack.
    auto book = std::make_unique<DefaultOrderBook>(1);
    auto handler = std::make_unique<DefaultMarketDataHandler>();
    auto gateway = std::make_unique<DefaultOrderGateway>();
    SPSCQueue<int, 8> queue;
    LinearArena<1024> arena;

    EXPECT_TRUE(book->empty());
    EXPECT_EQ(handler->queueDepth(), 0u);
    EXPECT_EQ(gateway->openOrderCount(), 0u);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(arena.used(), 0u);
}

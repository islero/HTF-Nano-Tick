/**
 * @file test_orderbook.cpp
 * @brief Unit tests for the OrderBook class.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#include <gtest/gtest.h>
#include <orderbook/OrderBook.hpp>

using namespace hft;

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        book = std::make_unique<DefaultOrderBook>(1);
    }

    std::unique_ptr<DefaultOrderBook> book;
};

//==============================================================================
// Basic Operations
//==============================================================================

TEST_F(OrderBookTest, InitialStateIsEmpty) {
    EXPECT_TRUE(book->empty());
    EXPECT_EQ(book->orderCount(), 0);
    EXPECT_EQ(book->bestBid(), INVALID_PRICE);
    EXPECT_EQ(book->bestAsk(), INVALID_PRICE);
}

TEST_F(OrderBookTest, AddSingleBidOrder) {
    Price price = priceFromDouble(100.50);
    Quantity qty = 100;

    EXPECT_TRUE(book->addOrder(1, Side::Buy, price, qty));

    EXPECT_FALSE(book->empty());
    EXPECT_EQ(book->orderCount(), 1);
    EXPECT_EQ(book->bestBid(), price);
    EXPECT_EQ(book->bestBidQty(), qty);
    EXPECT_EQ(book->bestAsk(), INVALID_PRICE);
}

TEST_F(OrderBookTest, AddSingleAskOrder) {
    Price price = priceFromDouble(100.55);
    Quantity qty = 200;

    EXPECT_TRUE(book->addOrder(1, Side::Sell, price, qty));

    EXPECT_FALSE(book->empty());
    EXPECT_EQ(book->orderCount(), 1);
    EXPECT_EQ(book->bestAsk(), price);
    EXPECT_EQ(book->bestAskQty(), qty);
    EXPECT_EQ(book->bestBid(), INVALID_PRICE);
}

TEST_F(OrderBookTest, AddMultipleBidLevels) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book->addOrder(2, Side::Buy, priceFromDouble(100.45), 200);
    (void)book->addOrder(3, Side::Buy, priceFromDouble(100.55), 150);  // Best bid

    EXPECT_EQ(book->orderCount(), 3);
    EXPECT_EQ(book->levelCount(Side::Buy), 3);
    EXPECT_EQ(book->bestBid(), priceFromDouble(100.55));
    EXPECT_EQ(book->bestBidQty(), 150);
}

TEST_F(OrderBookTest, AddMultipleAskLevels) {
    (void)book->addOrder(1, Side::Sell, priceFromDouble(100.60), 100);
    (void)book->addOrder(2, Side::Sell, priceFromDouble(100.65), 200);
    (void)book->addOrder(3, Side::Sell, priceFromDouble(100.55), 150);  // Best ask

    EXPECT_EQ(book->orderCount(), 3);
    EXPECT_EQ(book->levelCount(Side::Sell), 3);
    EXPECT_EQ(book->bestAsk(), priceFromDouble(100.55));
    EXPECT_EQ(book->bestAskQty(), 150);
}

TEST_F(OrderBookTest, AddMultipleOrdersAtSamePrice) {
    Price price = priceFromDouble(100.50);

    (void)book->addOrder(1, Side::Buy, price, 100);
    (void)book->addOrder(2, Side::Buy, price, 200);
    (void)book->addOrder(3, Side::Buy, price, 150);

    EXPECT_EQ(book->orderCount(), 3);
    EXPECT_EQ(book->levelCount(Side::Buy), 1);
    EXPECT_EQ(book->bestBidQty(), 450);  // 100 + 200 + 150
}

//==============================================================================
// Order Modification
//==============================================================================

TEST_F(OrderBookTest, ModifyOrderQuantity) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    EXPECT_EQ(book->bestBidQty(), 100);

    EXPECT_TRUE(book->modifyOrder(1, 200));
    EXPECT_EQ(book->bestBidQty(), 200);

    const Order* order = book->getOrder(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->quantity, 200);
}

TEST_F(OrderBookTest, ModifyOrderReduceQuantity) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book->addOrder(2, Side::Buy, priceFromDouble(100.50), 200);

    EXPECT_TRUE(book->modifyOrder(1, 50));
    EXPECT_EQ(book->bestBidQty(), 250);  // 50 + 200
}

TEST_F(OrderBookTest, ModifyNonExistentOrderFails) {
    EXPECT_FALSE(book->modifyOrder(999, 100));
}

TEST_F(OrderBookTest, ModifyToZeroDeletesOrder) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    EXPECT_TRUE(book->modifyOrder(1, 0));
    EXPECT_TRUE(book->empty());
    EXPECT_EQ(book->getOrder(1), nullptr);
}

//==============================================================================
// Order Deletion
//==============================================================================

TEST_F(OrderBookTest, DeleteOrder) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    EXPECT_TRUE(book->deleteOrder(1));
    EXPECT_TRUE(book->empty());
    EXPECT_EQ(book->getOrder(1), nullptr);
}

TEST_F(OrderBookTest, DeleteOrderUpdatesBBO) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.55), 100);  // Best
    (void)book->addOrder(2, Side::Buy, priceFromDouble(100.50), 200);

    EXPECT_EQ(book->bestBid(), priceFromDouble(100.55));

    EXPECT_TRUE(book->deleteOrder(1));

    EXPECT_EQ(book->bestBid(), priceFromDouble(100.50));
    EXPECT_EQ(book->bestBidQty(), 200);
}

TEST_F(OrderBookTest, DeleteNonExistentOrderFails) {
    EXPECT_FALSE(book->deleteOrder(999));
}

TEST_F(OrderBookTest, DeleteFromMultipleOrdersAtSamePrice) {
    Price price = priceFromDouble(100.50);
    (void)book->addOrder(1, Side::Buy, price, 100);
    (void)book->addOrder(2, Side::Buy, price, 200);
    (void)book->addOrder(3, Side::Buy, price, 150);

    EXPECT_TRUE(book->deleteOrder(2));

    EXPECT_EQ(book->orderCount(), 2);
    EXPECT_EQ(book->bestBidQty(), 250);  // 100 + 150
}

//==============================================================================
// Spread and Mid Price
//==============================================================================

TEST_F(OrderBookTest, CalculateSpread) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book->addOrder(2, Side::Sell, priceFromDouble(100.60), 100);

    EXPECT_EQ(book->spread(), priceFromDouble(0.10));
}

TEST_F(OrderBookTest, CalculateMidPrice) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book->addOrder(2, Side::Sell, priceFromDouble(100.60), 100);

    EXPECT_EQ(book->midPrice(), priceFromDouble(100.55));
}

TEST_F(OrderBookTest, SpreadInvalidWhenOneSideEmpty) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);

    EXPECT_EQ(book->spread(), INVALID_PRICE);
    EXPECT_EQ(book->midPrice(), INVALID_PRICE);
}

//==============================================================================
// Crossed Book Detection
//==============================================================================

TEST_F(OrderBookTest, DetectCrossedBook) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.60), 100);
    (void)book->addOrder(2, Side::Sell, priceFromDouble(100.50), 100);

    EXPECT_TRUE(book->isCrossed());
}

TEST_F(OrderBookTest, NotCrossedWhenNormal) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book->addOrder(2, Side::Sell, priceFromDouble(100.60), 100);

    EXPECT_FALSE(book->isCrossed());
}

//==============================================================================
// Edge Cases
//==============================================================================

TEST_F(OrderBookTest, RejectDuplicateOrderId) {
    EXPECT_TRUE(book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100));
    EXPECT_FALSE(book->addOrder(1, Side::Buy, priceFromDouble(100.55), 200));

    EXPECT_EQ(book->orderCount(), 1);
}

TEST_F(OrderBookTest, RejectInvalidPrice) {
    EXPECT_FALSE(book->addOrder(1, Side::Buy, INVALID_PRICE, 100));
    EXPECT_TRUE(book->empty());
}

TEST_F(OrderBookTest, RejectZeroQuantity) {
    EXPECT_FALSE(book->addOrder(1, Side::Buy, priceFromDouble(100.50), 0));
    EXPECT_TRUE(book->empty());
}

TEST_F(OrderBookTest, RejectNegativeQuantity) {
    EXPECT_FALSE(book->addOrder(1, Side::Buy, priceFromDouble(100.50), -100));
    EXPECT_TRUE(book->empty());
}

TEST_F(OrderBookTest, RejectInvalidOrderId) {
    EXPECT_FALSE(book->addOrder(INVALID_ORDER_ID, Side::Buy, priceFromDouble(100.50), 100));
    EXPECT_TRUE(book->empty());
}

TEST(OrderBookCapacityTest, RejectWhenMaxOrdersReached) {
    OrderBook<8, 3> smallBook(1);

    EXPECT_TRUE(smallBook.addOrder(1, Side::Buy, priceFromDouble(100.00), 10));
    EXPECT_TRUE(smallBook.addOrder(2, Side::Buy, priceFromDouble(100.01), 10));
    EXPECT_TRUE(smallBook.addOrder(3, Side::Sell, priceFromDouble(100.02), 10));
    EXPECT_FALSE(smallBook.addOrder(4, Side::Sell, priceFromDouble(100.03), 10));
    EXPECT_EQ(smallBook.orderCount(), 3u);
}

TEST(OrderBookCapacityTest, RejectWhenMaxLevelsReachedAndReuseReleasedSlot) {
    OrderBook<2, 4> smallBook(1);

    EXPECT_TRUE(smallBook.addOrder(1, Side::Buy, priceFromDouble(100.00), 10));
    EXPECT_TRUE(smallBook.addOrder(2, Side::Buy, priceFromDouble(99.99), 10));
    EXPECT_FALSE(smallBook.addOrder(3, Side::Buy, priceFromDouble(99.98), 10));

    EXPECT_EQ(smallBook.orderCount(), 2u);
    EXPECT_TRUE(smallBook.deleteOrder(2));
    EXPECT_TRUE(smallBook.addOrder(4, Side::Buy, priceFromDouble(99.98), 10));
    EXPECT_EQ(smallBook.orderCount(), 2u);
    EXPECT_EQ(smallBook.levelCount(Side::Buy), 2u);
}

//==============================================================================
// Snapshot
//==============================================================================

TEST_F(OrderBookTest, ApplySnapshot) {
    // Add some initial orders
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book->addOrder(2, Side::Sell, priceFromDouble(100.60), 100);

    // Apply snapshot (should clear and rebuild)
    std::vector<std::pair<Price, Quantity>> bids = {
        {priceFromDouble(99.50), 500},
        {priceFromDouble(99.40), 300}
    };
    std::vector<std::pair<Price, Quantity>> asks = {
        {priceFromDouble(99.60), 400},
        {priceFromDouble(99.70), 200}
    };

    book->applySnapshot(bids, asks);

    EXPECT_EQ(book->bestBid(), priceFromDouble(99.50));
    EXPECT_EQ(book->bestBidQty(), 500);
    EXPECT_EQ(book->bestAsk(), priceFromDouble(99.60));
    EXPECT_EQ(book->bestAskQty(), 400);
}

//==============================================================================
// Top Levels Query
//==============================================================================

TEST_F(OrderBookTest, GetTopBidLevels) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book->addOrder(2, Side::Buy, priceFromDouble(100.45), 200);
    (void)book->addOrder(3, Side::Buy, priceFromDouble(100.40), 150);
    (void)book->addOrder(4, Side::Buy, priceFromDouble(100.35), 250);

    std::vector<std::pair<Price, Quantity>> levels;
    book->getTopLevels(Side::Buy, 3, levels);

    ASSERT_EQ(levels.size(), 3);
    EXPECT_EQ(levels[0].first, priceFromDouble(100.50));
    EXPECT_EQ(levels[0].second, 100);
    EXPECT_EQ(levels[1].first, priceFromDouble(100.45));
    EXPECT_EQ(levels[1].second, 200);
    EXPECT_EQ(levels[2].first, priceFromDouble(100.40));
    EXPECT_EQ(levels[2].second, 150);
}

TEST_F(OrderBookTest, GetTopAskLevels) {
    (void)book->addOrder(1, Side::Sell, priceFromDouble(100.60), 100);
    (void)book->addOrder(2, Side::Sell, priceFromDouble(100.65), 200);
    (void)book->addOrder(3, Side::Sell, priceFromDouble(100.70), 150);
    (void)book->addOrder(4, Side::Sell, priceFromDouble(100.75), 250);

    std::vector<std::pair<Price, Quantity>> levels;
    book->getTopLevels(Side::Sell, 3, levels);

    ASSERT_EQ(levels.size(), 3);
    EXPECT_EQ(levels[0].first, priceFromDouble(100.60));
    EXPECT_EQ(levels[0].second, 100);
    EXPECT_EQ(levels[1].first, priceFromDouble(100.65));
    EXPECT_EQ(levels[1].second, 200);
    EXPECT_EQ(levels[2].first, priceFromDouble(100.70));
    EXPECT_EQ(levels[2].second, 150);
}

//==============================================================================
// Callback Tests
//==============================================================================

TEST_F(OrderBookTest, UpdateCallbackOnAdd) {
    int callbackCount = 0;
    OrderBookUpdate lastUpdate;

    book->setUpdateCallback([&](const OrderBookUpdate& update) {
        ++callbackCount;
        lastUpdate = update;
    });

    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(lastUpdate.action, MdMsgType::Add);
    EXPECT_EQ(lastUpdate.side, Side::Buy);
    EXPECT_EQ(lastUpdate.price, priceFromDouble(100.50));
    EXPECT_EQ(lastUpdate.quantity, 100);
}

TEST_F(OrderBookTest, UpdateCallbackOnModify) {
    int callbackCount = 0;
    OrderBookUpdate lastUpdate;

    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);

    book->setUpdateCallback([&](const OrderBookUpdate& update) {
        ++callbackCount;
        lastUpdate = update;
    });

    (void)book->modifyOrder(1, 200);

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(lastUpdate.action, MdMsgType::Modify);
    EXPECT_EQ(lastUpdate.quantity, 200);
}

TEST_F(OrderBookTest, UpdateCallbackOnDelete) {
    int callbackCount = 0;
    OrderBookUpdate lastUpdate;

    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);

    book->setUpdateCallback([&](const OrderBookUpdate& update) {
        ++callbackCount;
        lastUpdate = update;
    });

    (void)book->deleteOrder(1);

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(lastUpdate.action, MdMsgType::Delete);
}

//==============================================================================
// Clear
//==============================================================================

TEST_F(OrderBookTest, ClearBook) {
    (void)book->addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book->addOrder(2, Side::Sell, priceFromDouble(100.60), 100);

    EXPECT_FALSE(book->empty());

    book->clear();

    EXPECT_TRUE(book->empty());
    EXPECT_EQ(book->bestBid(), INVALID_PRICE);
    EXPECT_EQ(book->bestAsk(), INVALID_PRICE);
}

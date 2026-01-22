/**
 * @file test_strategy.cpp
 * @brief Unit tests for the Strategy Engine.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#include <gtest/gtest.h>
#include <strategy/StrategyEngine.hpp>
#include <orderbook/OrderBook.hpp>

using namespace hft;

class CashCarryStrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        spotBook = std::make_unique<DefaultOrderBook>(1);
        futuresBook = std::make_unique<DefaultOrderBook>(2);

        config.entryThreshold = priceFromDouble(50.0);  // $50
        config.exitThreshold = priceFromDouble(5.0);    // $5
        config.defaultQty = 10;
        config.maxPosition = 100;

        strategy = std::make_unique<CashCarryArbitrage>(
            spotBook.get(), futuresBook.get(), config, 1
        );
    }

    void setupNormalMarket() {
        // Spot: 50000 / 50010
        (void)spotBook->addOrder(1, Side::Buy, priceFromDouble(50000.0), 100);
        (void)spotBook->addOrder(2, Side::Sell, priceFromDouble(50010.0), 100);

        // Futures: 50020 / 50030
        (void)futuresBook->addOrder(3, Side::Buy, priceFromDouble(50020.0), 100);
        (void)futuresBook->addOrder(4, Side::Sell, priceFromDouble(50030.0), 100);
    }

    void setupArbitrageOpportunity() {
        // Spot: 50000 / 50010
        (void)spotBook->addOrder(1, Side::Buy, priceFromDouble(50000.0), 100);
        (void)spotBook->addOrder(2, Side::Sell, priceFromDouble(50010.0), 100);

        // Futures with large premium: 50100 / 50110 (basis = $90)
        (void)futuresBook->addOrder(3, Side::Buy, priceFromDouble(50100.0), 100);
        (void)futuresBook->addOrder(4, Side::Sell, priceFromDouble(50110.0), 100);
    }

    void setupReverseArbitrageOpportunity() {
        // Spot with premium: 50100 / 50110
        (void)spotBook->addOrder(1, Side::Buy, priceFromDouble(50100.0), 100);
        (void)spotBook->addOrder(2, Side::Sell, priceFromDouble(50110.0), 100);

        // Futures: 50000 / 50010 (spot premium = $90)
        (void)futuresBook->addOrder(3, Side::Buy, priceFromDouble(50000.0), 100);
        (void)futuresBook->addOrder(4, Side::Sell, priceFromDouble(50010.0), 100);
    }

    std::unique_ptr<DefaultOrderBook> spotBook;
    std::unique_ptr<DefaultOrderBook> futuresBook;
    std::unique_ptr<CashCarryArbitrage> strategy;
    CashCarryConfig config;
};

//==============================================================================
// Initialization
//==============================================================================

TEST_F(CashCarryStrategyTest, InitializeSuccess) {
    EXPECT_TRUE(strategy->initialize());
    EXPECT_TRUE(strategy->isActive());
    EXPECT_EQ(strategy->state(), StrategyState::Active);
}

TEST_F(CashCarryStrategyTest, InitializeFailsWithNullBooks) {
    CashCarryArbitrage badStrategy(nullptr, futuresBook.get(), config, 1);
    EXPECT_FALSE(badStrategy.initialize());
    EXPECT_EQ(badStrategy.state(), StrategyState::Error);
}

TEST_F(CashCarryStrategyTest, StopStrategy) {
    (void)strategy->initialize();
    strategy->stop();
    EXPECT_EQ(strategy->state(), StrategyState::Stopped);
    EXPECT_FALSE(strategy->isActive());
}

TEST_F(CashCarryStrategyTest, PauseAndResume) {
    (void)strategy->initialize();

    strategy->pause();
    EXPECT_EQ(strategy->state(), StrategyState::Paused);
    EXPECT_FALSE(strategy->isActive());

    strategy->resume();
    EXPECT_EQ(strategy->state(), StrategyState::Active);
    EXPECT_TRUE(strategy->isActive());
}

//==============================================================================
// Signal Generation - No Opportunity
//==============================================================================

TEST_F(CashCarryStrategyTest, NoSignalWhenNotInitialized) {
    setupArbitrageOpportunity();

    OrderBookUpdate update;
    auto orders = strategy->onMarketData(update);

    EXPECT_TRUE(orders.empty());
    EXPECT_EQ(strategy->stats().signalsGenerated.load(), 0);
}

TEST_F(CashCarryStrategyTest, NoSignalWhenBelowThreshold) {
    setupNormalMarket();  // Basis = $10, threshold = $50
    (void)strategy->initialize();

    OrderBookUpdate update;
    update.action = MdMsgType::Modify;
    update.side = Side::Buy;

    auto orders = strategy->onMarketData(update);

    EXPECT_TRUE(orders.empty());
    EXPECT_EQ(strategy->stats().ticksProcessed.load(), 1);
    EXPECT_EQ(strategy->stats().signalsGenerated.load(), 0);
}

TEST_F(CashCarryStrategyTest, NoSignalWithEmptyBooks) {
    (void)strategy->initialize();

    OrderBookUpdate update;
    auto orders = strategy->onMarketData(update);

    EXPECT_TRUE(orders.empty());
}

//==============================================================================
// Signal Generation - Arbitrage Opportunities
//==============================================================================

TEST_F(CashCarryStrategyTest, GenerateSignalForCashAndCarry) {
    setupArbitrageOpportunity();  // Futures premium = $90
    (void)strategy->initialize();

    OrderBookUpdate update;
    update.action = MdMsgType::Add;
    update.side = Side::Sell;

    auto orders = strategy->onMarketData(update);

    // Should generate buy spot / sell futures
    ASSERT_EQ(orders.size(), 2);
    EXPECT_EQ(strategy->stats().signalsGenerated.load(), 1);

    // First order: Buy Spot at ask
    EXPECT_EQ(orders[0].symbolId, spotBook->symbolId());
    EXPECT_EQ(orders[0].side, Side::Buy);
    EXPECT_EQ(orders[0].price, spotBook->bestAsk());
    EXPECT_EQ(orders[0].quantity, config.defaultQty);

    // Second order: Sell Futures at bid
    EXPECT_EQ(orders[1].symbolId, futuresBook->symbolId());
    EXPECT_EQ(orders[1].side, Side::Sell);
    EXPECT_EQ(orders[1].price, futuresBook->bestBid());
    EXPECT_EQ(orders[1].quantity, config.defaultQty);
}

TEST_F(CashCarryStrategyTest, GenerateSignalForReverseCashAndCarry) {
    setupReverseArbitrageOpportunity();  // Spot premium = $90
    (void)strategy->initialize();

    OrderBookUpdate update;
    update.action = MdMsgType::Add;
    update.side = Side::Sell;

    auto orders = strategy->onMarketData(update);

    // Should generate sell spot / buy futures
    ASSERT_EQ(orders.size(), 2);

    // First order: Sell Spot at bid
    EXPECT_EQ(orders[0].symbolId, spotBook->symbolId());
    EXPECT_EQ(orders[0].side, Side::Sell);
    EXPECT_EQ(orders[0].price, spotBook->bestBid());

    // Second order: Buy Futures at ask
    EXPECT_EQ(orders[1].symbolId, futuresBook->symbolId());
    EXPECT_EQ(orders[1].side, Side::Buy);
    EXPECT_EQ(orders[1].price, futuresBook->bestAsk());
}

//==============================================================================
// Position Tracking
//==============================================================================

TEST_F(CashCarryStrategyTest, PositionUpdatedOnFill) {
    setupArbitrageOpportunity();
    (void)strategy->initialize();

    EXPECT_EQ(strategy->spotPosition(), 0);
    EXPECT_EQ(strategy->futuresPosition(), 0);

    // First trigger a signal via market data to set m_lastSignal
    OrderBookUpdate update;
    update.action = MdMsgType::Add;
    update.side = Side::Sell;
    auto orders = strategy->onMarketData(update);
    ASSERT_FALSE(orders.empty());  // Verify signal was generated

    // Now simulate fill (updates positions based on last signal)
    strategy->handleOrderFill(1, priceFromDouble(50010.0), 10);

    EXPECT_EQ(strategy->spotPosition(), 10);
    EXPECT_EQ(strategy->futuresPosition(), -10);
}

TEST_F(CashCarryStrategyTest, StatisticsUpdatedOnFill) {
    (void)strategy->initialize();

    strategy->handleOrderFill(1, priceFromDouble(50000.0), 100);

    EXPECT_EQ(strategy->stats().ordersFilled.load(), 1);
}

TEST_F(CashCarryStrategyTest, StatisticsUpdatedOnReject) {
    (void)strategy->initialize();

    strategy->handleOrderReject(1, 1);

    EXPECT_EQ(strategy->stats().ordersRejected.load(), 1);
}

//==============================================================================
// Basis Calculation
//==============================================================================

TEST_F(CashCarryStrategyTest, CurrentBasisCalculation) {
    setupArbitrageOpportunity();
    (void)strategy->initialize();

    // Trigger a tick to update internal state
    OrderBookUpdate update;
    strategy->onMarketData(update);

    // Basis = Futures Bid - Spot Ask = 50100 - 50010 = 90
    Price expectedBasis = priceFromDouble(90.0);
    EXPECT_EQ(strategy->currentBasis(), expectedBasis);
}

TEST_F(CashCarryStrategyTest, BasisInvalidWithEmptyBooks) {
    (void)strategy->initialize();

    EXPECT_EQ(strategy->currentBasis(), INVALID_PRICE);
}

//==============================================================================
// Configuration
//==============================================================================

TEST_F(CashCarryStrategyTest, ConfigurationAccessible) {
    const auto& cfg = strategy->config();

    EXPECT_EQ(cfg.entryThreshold, config.entryThreshold);
    EXPECT_EQ(cfg.exitThreshold, config.exitThreshold);
    EXPECT_EQ(cfg.defaultQty, config.defaultQty);
}

TEST_F(CashCarryStrategyTest, UpdateConfiguration) {
    CashCarryConfig newConfig;
    newConfig.entryThreshold = priceFromDouble(100.0);
    newConfig.defaultQty = 50;

    strategy->setConfig(newConfig);

    EXPECT_EQ(strategy->config().entryThreshold, priceFromDouble(100.0));
    EXPECT_EQ(strategy->config().defaultQty, 50);
}

//==============================================================================
// Order Request Properties
//==============================================================================

TEST_F(CashCarryStrategyTest, OrderRequestHasStrategyId) {
    setupArbitrageOpportunity();
    (void)strategy->initialize();

    OrderBookUpdate update;
    auto orders = strategy->onMarketData(update);

    ASSERT_FALSE(orders.empty());

    for (const auto& order : orders) {
        EXPECT_EQ(order.strategyId, strategy->strategyId());
    }
}

TEST_F(CashCarryStrategyTest, OrderRequestHasTimestamp) {
    setupArbitrageOpportunity();
    (void)strategy->initialize();

    Timestamp before = nowNanos();

    OrderBookUpdate update;
    auto orders = strategy->onMarketData(update);

    Timestamp after = nowNanos();

    ASSERT_FALSE(orders.empty());

    for (const auto& order : orders) {
        EXPECT_GE(order.requestTime, before);
        EXPECT_LE(order.requestTime, after);
    }
}

TEST_F(CashCarryStrategyTest, OrderRequestHasCorrectType) {
    setupArbitrageOpportunity();
    (void)strategy->initialize();

    OrderBookUpdate update;
    auto orders = strategy->onMarketData(update);

    ASSERT_FALSE(orders.empty());

    for (const auto& order : orders) {
        EXPECT_EQ(order.orderType, OrderType::Limit);
    }
}

//==============================================================================
// Statistics
//==============================================================================

TEST_F(CashCarryStrategyTest, TickCountIncremented) {
    setupNormalMarket();
    (void)strategy->initialize();

    OrderBookUpdate update;

    for (int i = 0; i < 10; ++i) {
        strategy->onMarketData(update);
    }

    EXPECT_EQ(strategy->stats().ticksProcessed.load(), 10);
}

TEST_F(CashCarryStrategyTest, StatsResetOnInitialize) {
    setupArbitrageOpportunity();
    (void)strategy->initialize();

    OrderBookUpdate update;
    strategy->onMarketData(update);

    EXPECT_GT(strategy->stats().signalsGenerated.load(), 0);

    // Re-initialize
    strategy->stop();
    (void)strategy->initialize();

    EXPECT_EQ(strategy->stats().signalsGenerated.load(), 0);
    EXPECT_EQ(strategy->stats().ticksProcessed.load(), 0);
}

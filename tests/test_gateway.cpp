/**
 * @file test_gateway.cpp
 * @brief Unit tests for protocol-neutral order gateway behavior.
 */

#include <gtest/gtest.h>
#include <gateway/OrderEntryGateway.hpp>

#include <limits>
#include <memory>
#include <vector>

using namespace hft;

class OrderGatewayTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.maxPositionPerSymbol = 100;
        config.maxOrderValue = 1'000'000'000'000;
        gateway = std::make_unique<DefaultOrderGateway>(config);
        gateway->setSendCallback([this](const OutboundOrderMessage& message) {
            sentMessages.push_back(message);
            return true;
        });
    }

    [[nodiscard]] static OrderRequest makeRequest(Side side, Quantity qty) noexcept {
        OrderRequest request;
        request.symbolId = 7;
        request.side = side;
        request.price = priceFromDouble(100.0);
        request.quantity = qty;
        request.orderType = OrderType::Limit;
        request.strategyId = 42;
        request.requestTime = nowNanos();
        return request;
    }

    GatewayConfig config;
    std::unique_ptr<DefaultOrderGateway> gateway;
    std::vector<OutboundOrderMessage> sentMessages;
};

TEST_F(OrderGatewayTest, SubmitOrderEmitsProtocolNeutralNewOrder) {
    OrderId orderId = gateway->submitOrder(makeRequest(Side::Buy, 25));

    ASSERT_NE(orderId, INVALID_ORDER_ID);
    ASSERT_EQ(sentMessages.size(), 1u);
    EXPECT_EQ(sentMessages[0].action, OutboundOrderAction::New);
    EXPECT_EQ(sentMessages[0].clientOrderId, orderId);
    EXPECT_EQ(sentMessages[0].symbolId, 7u);
    EXPECT_EQ(sentMessages[0].side, Side::Buy);
    EXPECT_EQ(sentMessages[0].quantity, 25);
}

TEST_F(OrderGatewayTest, PendingExposureCountsAgainstPositionLimit) {
    OrderId firstOrderId = gateway->submitOrder(makeRequest(Side::Buy, 60));
    OrderId secondOrderId = gateway->submitOrder(makeRequest(Side::Buy, 50));

    EXPECT_NE(firstOrderId, INVALID_ORDER_ID);
    EXPECT_EQ(secondOrderId, INVALID_ORDER_ID);
    EXPECT_EQ(gateway->stats().riskRejections.load(), 1u);
    EXPECT_EQ(sentMessages.size(), 1u);
}

TEST_F(OrderGatewayTest, CancelOrderEmitsProtocolNeutralCancel) {
    OrderId orderId = gateway->submitOrder(makeRequest(Side::Sell, 25));
    ASSERT_NE(orderId, INVALID_ORDER_ID);

    sentMessages.clear();
    EXPECT_TRUE(gateway->cancelOrder(orderId));

    ASSERT_EQ(sentMessages.size(), 1u);
    EXPECT_EQ(sentMessages[0].action, OutboundOrderAction::Cancel);
    EXPECT_EQ(sentMessages[0].origClientOrderId, orderId);
    EXPECT_EQ(sentMessages[0].side, Side::Sell);
}

TEST_F(OrderGatewayTest, RejectsOverflowProneOrderValue) {
    OrderRequest request = makeRequest(Side::Buy, std::numeric_limits<Quantity>::max());
    request.price = std::numeric_limits<Price>::max();

    EXPECT_EQ(gateway->submitOrder(request), INVALID_ORDER_ID);
    EXPECT_EQ(gateway->stats().riskRejections.load(), 1u);
}

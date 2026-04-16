/**
 * @file test_fix_parser.cpp
 * @brief Unit tests for the QuickFIX integration.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#include <gtest/gtest.h>
#include <market_data/QuickFixApplication.hpp>
#include <market_data/MarketDataHandler.hpp>
#include <gateway/OrderEntryGateway.hpp>

#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/MarketDataSnapshotFullRefresh.h>
#include <quickfix/fix44/MarketDataIncrementalRefresh.h>

#include <string>

using namespace hft;

//==============================================================================
// QuickFIX Message Building Tests
//==============================================================================

class QuickFixMessageTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common setup if needed
    }
};

TEST_F(QuickFixMessageTest, BuildNewOrderSingle) {
    FIX44::NewOrderSingle order;

    order.set(FIX::ClOrdID("12345"));
    order.set(FIX::Symbol("BTCUSD"));
    order.set(FIX::Side(qfix::SIDE_BUY));
    order.set(FIX::TransactTime(FIX::UtcTimeStamp()));
    order.set(FIX::OrdType(qfix::ORD_TYPE_LIMIT));
    order.set(FIX::Price(50000.50));
    order.set(FIX::OrderQty(100));
    order.set(FIX::TimeInForce(qfix::TIF_DAY));

    // Verify fields
    FIX::ClOrdID clOrdId;
    order.get(clOrdId);
    EXPECT_EQ(clOrdId.getValue(), "12345");

    FIX::Symbol symbol;
    order.get(symbol);
    EXPECT_EQ(symbol.getValue(), "BTCUSD");

    FIX::Side side;
    order.get(side);
    EXPECT_EQ(side.getValue(), qfix::SIDE_BUY);

    FIX::Price price;
    order.get(price);
    EXPECT_DOUBLE_EQ(price.getValue(), 50000.50);

    FIX::OrderQty qty;
    order.get(qty);
    EXPECT_DOUBLE_EQ(qty.getValue(), 100.0);
}

TEST_F(QuickFixMessageTest, BuildOrderCancelRequest) {
    FIX44::OrderCancelRequest cancel;

    cancel.set(FIX::ClOrdID("12346"));
    cancel.set(FIX::OrigClOrdID("12345"));
    cancel.set(FIX::Symbol("BTCUSD"));
    cancel.set(FIX::Side(qfix::SIDE_BUY));
    cancel.set(FIX::TransactTime(FIX::UtcTimeStamp()));

    FIX::OrigClOrdID origClOrdId;
    cancel.get(origClOrdId);
    EXPECT_EQ(origClOrdId.getValue(), "12345");
}

TEST_F(QuickFixMessageTest, BuildSellOrder) {
    FIX44::NewOrderSingle order;

    order.set(FIX::ClOrdID("99999"));
    order.set(FIX::Symbol("ETHUSD"));
    order.set(FIX::Side(qfix::SIDE_SELL));
    order.set(FIX::TransactTime(FIX::UtcTimeStamp()));
    order.set(FIX::OrdType(qfix::ORD_TYPE_LIMIT));
    order.set(FIX::Price(3000.25));
    order.set(FIX::OrderQty(50));

    FIX::Side side;
    order.get(side);
    EXPECT_EQ(side.getValue(), qfix::SIDE_SELL);
}

//==============================================================================
// Execution Report Tests
//==============================================================================

TEST_F(QuickFixMessageTest, ParseExecutionReport_New) {
    FIX44::ExecutionReport report;

    report.set(FIX::ClOrdID("12345"));
    report.set(FIX::OrderID("EX123456"));
    report.set(FIX::ExecID("EXEC001"));
    report.set(FIX::ExecType(qfix::EXEC_TYPE_NEW));
    report.set(FIX::OrdStatus('0')); // New
    report.set(FIX::Symbol("BTCUSD"));
    report.set(FIX::Side(qfix::SIDE_BUY));
    report.set(FIX::LeavesQty(100));
    report.set(FIX::CumQty(0));

    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(execType.getValue(), qfix::EXEC_TYPE_NEW);

    FIX::LeavesQty leavesQty;
    report.get(leavesQty);
    EXPECT_DOUBLE_EQ(leavesQty.getValue(), 100.0);
}

TEST_F(QuickFixMessageTest, ParseExecutionReport_PartialFill) {
    FIX44::ExecutionReport report;

    report.set(FIX::ClOrdID("12345"));
    report.set(FIX::OrderID("EX123456"));
    report.set(FIX::ExecID("EXEC002"));
    report.set(FIX::ExecType(qfix::EXEC_TYPE_PARTIAL_FILL));
    report.set(FIX::OrdStatus('1')); // Partially filled
    report.set(FIX::Symbol("BTCUSD"));
    report.set(FIX::Side(qfix::SIDE_BUY));
    report.set(FIX::LastPx(50000.50));
    report.set(FIX::LastQty(25));
    report.set(FIX::LeavesQty(75));
    report.set(FIX::CumQty(25));

    FIX::LastPx lastPx;
    report.get(lastPx);
    EXPECT_DOUBLE_EQ(lastPx.getValue(), 50000.50);

    FIX::LastQty lastQty;
    report.get(lastQty);
    EXPECT_DOUBLE_EQ(lastQty.getValue(), 25.0);

    FIX::CumQty cumQty;
    report.get(cumQty);
    EXPECT_DOUBLE_EQ(cumQty.getValue(), 25.0);
}

TEST_F(QuickFixMessageTest, ParseExecutionReport_Fill) {
    FIX44::ExecutionReport report;

    report.set(FIX::ClOrdID("12345"));
    report.set(FIX::OrderID("EX123456"));
    report.set(FIX::ExecID("EXEC003"));
    report.set(FIX::ExecType(qfix::EXEC_TYPE_FILL));
    report.set(FIX::OrdStatus('2')); // Filled
    report.set(FIX::Symbol("BTCUSD"));
    report.set(FIX::Side(qfix::SIDE_BUY));
    report.set(FIX::LastPx(50000.50));
    report.set(FIX::LastQty(100));
    report.set(FIX::LeavesQty(0));
    report.set(FIX::CumQty(100));

    FIX::ExecType execType;
    report.get(execType);
    EXPECT_EQ(execType.getValue(), qfix::EXEC_TYPE_FILL);

    FIX::LeavesQty leavesQty;
    report.get(leavesQty);
    EXPECT_DOUBLE_EQ(leavesQty.getValue(), 0.0);
}

TEST_F(QuickFixMessageTest, ParseExecutionReport_Rejected) {
    FIX44::ExecutionReport report;

    report.set(FIX::ClOrdID("12345"));
    report.set(FIX::ExecID("EXEC004"));
    report.set(FIX::ExecType(qfix::EXEC_TYPE_REJECTED));
    report.set(FIX::OrdStatus('8')); // Rejected
    report.set(FIX::Symbol("BTCUSD"));
    report.set(FIX::Side(qfix::SIDE_BUY));
    report.set(FIX::LeavesQty(0));
    report.set(FIX::CumQty(0));
    report.set(FIX::Text("Insufficient funds"));

    FIX::Text text;
    report.get(text);
    EXPECT_EQ(text.getValue(), "Insufficient funds");
}

//==============================================================================
// Market Data Message Tests
//==============================================================================

TEST_F(QuickFixMessageTest, BuildMarketDataSnapshot) {
    FIX44::MarketDataSnapshotFullRefresh snapshot;

    snapshot.set(FIX::Symbol("BTCUSD"));

    // Add bid entry
    FIX44::MarketDataSnapshotFullRefresh::NoMDEntries bidGroup;
    bidGroup.set(FIX::MDEntryType(qfix::MD_ENTRY_BID));
    bidGroup.set(FIX::MDEntryPx(50000.00));
    bidGroup.set(FIX::MDEntrySize(10));
    snapshot.addGroup(bidGroup);

    // Add ask entry
    FIX44::MarketDataSnapshotFullRefresh::NoMDEntries askGroup;
    askGroup.set(FIX::MDEntryType(qfix::MD_ENTRY_ASK));
    askGroup.set(FIX::MDEntryPx(50010.00));
    askGroup.set(FIX::MDEntrySize(5));
    snapshot.addGroup(askGroup);

    // Verify
    FIX::Symbol symbol;
    snapshot.get(symbol);
    EXPECT_EQ(symbol.getValue(), "BTCUSD");

    EXPECT_EQ(snapshot.groupCount(FIX::FIELD::NoMDEntries), 2u);
}

TEST_F(QuickFixMessageTest, ParseMarketDataSnapshot) {
    FIX44::MarketDataSnapshotFullRefresh snapshot;

    snapshot.set(FIX::Symbol("BTCUSD"));

    // Add entries
    FIX44::MarketDataSnapshotFullRefresh::NoMDEntries bidGroup;
    bidGroup.set(FIX::MDEntryType(qfix::MD_ENTRY_BID));
    bidGroup.set(FIX::MDEntryPx(50000.00));
    bidGroup.set(FIX::MDEntrySize(10));
    snapshot.addGroup(bidGroup);

    FIX44::MarketDataSnapshotFullRefresh::NoMDEntries askGroup;
    askGroup.set(FIX::MDEntryType(qfix::MD_ENTRY_ASK));
    askGroup.set(FIX::MDEntryPx(50010.00));
    askGroup.set(FIX::MDEntrySize(5));
    snapshot.addGroup(askGroup);

    // Parse entries
    int numEntries = static_cast<int>(snapshot.groupCount(FIX::FIELD::NoMDEntries));
    EXPECT_EQ(numEntries, 2);

    FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
    snapshot.getGroup(1, group);

    FIX::MDEntryType entryType;
    group.get(entryType);
    EXPECT_EQ(entryType.getValue(), qfix::MD_ENTRY_BID);

    FIX::MDEntryPx px;
    group.get(px);
    EXPECT_DOUBLE_EQ(px.getValue(), 50000.00);

    FIX::MDEntrySize size;
    group.get(size);
    EXPECT_DOUBLE_EQ(size.getValue(), 10.0);
}

TEST_F(QuickFixMessageTest, BuildMarketDataIncremental) {
    FIX44::MarketDataIncrementalRefresh incremental;

    // Add new bid
    FIX44::MarketDataIncrementalRefresh::NoMDEntries entry;
    entry.set(FIX::MDUpdateAction(qfix::MD_ACTION_NEW));
    entry.set(FIX::MDEntryType(qfix::MD_ENTRY_BID));
    entry.set(FIX::Symbol("BTCUSD"));
    entry.set(FIX::MDEntryPx(49999.00));
    entry.set(FIX::MDEntrySize(20));
    incremental.addGroup(entry);

    // Add delete
    FIX44::MarketDataIncrementalRefresh::NoMDEntries deleteEntry;
    deleteEntry.set(FIX::MDUpdateAction(qfix::MD_ACTION_DELETE));
    deleteEntry.set(FIX::MDEntryType(qfix::MD_ENTRY_ASK));
    deleteEntry.set(FIX::Symbol("BTCUSD"));
    deleteEntry.set(FIX::MDEntryID("ASK001"));
    incremental.addGroup(deleteEntry);

    EXPECT_EQ(incremental.groupCount(FIX::FIELD::NoMDEntries), 2u);
}

//==============================================================================
// Type Conversion Tests
//==============================================================================

class TypeConversionTest : public ::testing::Test {
protected:
};

TEST_F(TypeConversionTest, PriceConversion) {
    double originalPrice = 50000.12345678;

    Price fixedPoint = priceFromDouble(originalPrice);
    double backToDouble = priceToDouble(fixedPoint);

    EXPECT_NEAR(backToDouble, originalPrice, 0.00000001);
}

TEST_F(TypeConversionTest, PriceConversionNegative) {
    double originalPrice = -123.45;

    Price fixedPoint = priceFromDouble(originalPrice);
    double backToDouble = priceToDouble(fixedPoint);

    EXPECT_NEAR(backToDouble, originalPrice, 0.00000001);
}

TEST_F(TypeConversionTest, PriceConversionSmall) {
    double originalPrice = 0.00000001;

    Price fixedPoint = priceFromDouble(originalPrice);
    double backToDouble = priceToDouble(fixedPoint);

    EXPECT_NEAR(backToDouble, originalPrice, 0.000000001);
}

TEST_F(TypeConversionTest, SideMapping) {
    // Buy side
    Side buyLocal = Side::Buy;
    char buyFix = (buyLocal == Side::Buy) ? qfix::SIDE_BUY : qfix::SIDE_SELL;
    EXPECT_EQ(buyFix, '1');

    // Sell side
    Side sellLocal = Side::Sell;
    char sellFix = (sellLocal == Side::Buy) ? qfix::SIDE_BUY : qfix::SIDE_SELL;
    EXPECT_EQ(sellFix, '2');
}

TEST_F(TypeConversionTest, ExecTypeMapping) {
    // Verify exec type constants
    EXPECT_EQ(qfix::EXEC_TYPE_NEW, '0');
    EXPECT_EQ(qfix::EXEC_TYPE_PARTIAL_FILL, '1');
    EXPECT_EQ(qfix::EXEC_TYPE_FILL, '2');
    EXPECT_EQ(qfix::EXEC_TYPE_CANCELED, '4');
    EXPECT_EQ(qfix::EXEC_TYPE_REJECTED, '8');
}

TEST_F(TypeConversionTest, MdEntryTypeMapping) {
    EXPECT_EQ(qfix::MD_ENTRY_BID, '0');
    EXPECT_EQ(qfix::MD_ENTRY_ASK, '1');
    EXPECT_EQ(qfix::MD_ENTRY_TRADE, '2');
}

TEST_F(TypeConversionTest, MdUpdateActionMapping) {
    EXPECT_EQ(qfix::MD_ACTION_NEW, '0');
    EXPECT_EQ(qfix::MD_ACTION_CHANGE, '1');
    EXPECT_EQ(qfix::MD_ACTION_DELETE, '2');
}

//==============================================================================
// QuickFixMdEntry Tests
//==============================================================================

TEST_F(QuickFixMessageTest, QuickFixMdEntry) {
    QuickFixMdEntry entry;
    entry.entryType = qfix::MD_ENTRY_BID;
    entry.updateAction = qfix::MD_ACTION_NEW;
    entry.price = priceFromDouble(50000.0);
    entry.size = 100;
    entry.entryId = "BID001";
    entry.receiveTime = nowNanos();

    EXPECT_EQ(entry.entryType, qfix::MD_ENTRY_BID);
    EXPECT_EQ(entry.updateAction, qfix::MD_ACTION_NEW);
    EXPECT_EQ(priceToDouble(entry.price), 50000.0);
    EXPECT_EQ(entry.size, 100u);
    EXPECT_EQ(entry.entryId, "BID001");
}

//==============================================================================
// QuickFixExecReport Tests
//==============================================================================

TEST_F(QuickFixMessageTest, QuickFixExecReport) {
    QuickFixExecReport report;
    report.clOrdId = 12345;
    report.orderId = 99999;
    report.execType = qfix::EXEC_TYPE_FILL;
    report.status = OrderStatus::Filled;
    report.lastPx = priceFromDouble(50000.50);
    report.lastQty = 100;
    report.cumQty = 100;
    report.leavesQty = 0;
    report.text = "";
    report.receiveTime = nowNanos();

    EXPECT_EQ(report.clOrdId, 12345u);
    EXPECT_EQ(report.orderId, 99999u);
    EXPECT_EQ(report.execType, qfix::EXEC_TYPE_FILL);
    EXPECT_EQ(report.status, OrderStatus::Filled);
    EXPECT_EQ(priceToDouble(report.lastPx), 50000.50);
}

//==============================================================================
// Message Serialization Tests
//==============================================================================

TEST_F(QuickFixMessageTest, MessageToString) {
    FIX44::NewOrderSingle order;

    order.set(FIX::ClOrdID("12345"));
    order.set(FIX::Symbol("BTCUSD"));
    order.set(FIX::Side(qfix::SIDE_BUY));
    order.set(FIX::TransactTime(FIX::UtcTimeStamp()));
    order.set(FIX::OrdType(qfix::ORD_TYPE_LIMIT));
    order.set(FIX::Price(50000.50));
    order.set(FIX::OrderQty(100));

    std::string msgStr = order.toString();

    // Message should contain key fields
    EXPECT_NE(msgStr.find("35=D"), std::string::npos); // MsgType
    EXPECT_NE(msgStr.find("11=12345"), std::string::npos); // ClOrdID
    EXPECT_NE(msgStr.find("55=BTCUSD"), std::string::npos); // Symbol
}

//==============================================================================
// Order Entry Gateway Integration Tests
//==============================================================================

class GatewayQuickFixTest : public ::testing::Test {
protected:
    DefaultOrderGateway gateway;
    std::vector<FIX::Message> sentMessages;

    void SetUp() override {
        gateway.setSendCallback([this](FIX::Message& msg) -> bool {
            sentMessages.push_back(msg);
            return true;
        });
    }
};

TEST_F(GatewayQuickFixTest, SubmitOrderSendsQuickFixMessage) {
    OrderRequest request;
    request.symbolId = 1;
    request.side = Side::Buy;
    request.price = priceFromDouble(50000.0);
    request.quantity = 100;
    request.orderType = OrderType::Limit;
    request.strategyId = 1;
    request.requestTime = nowNanos();

    OrderId orderId = gateway.submitOrder(request);

    EXPECT_NE(orderId, INVALID_ORDER_ID);
    EXPECT_EQ(sentMessages.size(), 1u);

    // Verify the message is a NewOrderSingle
    FIX::MsgType msgType;
    sentMessages[0].getHeader().get(msgType);
    EXPECT_EQ(msgType.getValue(), "D");
}

TEST_F(GatewayQuickFixTest, CancelOrderSendsQuickFixMessage) {
    // First submit an order
    OrderRequest request;
    request.symbolId = 1;
    request.side = Side::Buy;
    request.price = priceFromDouble(50000.0);
    request.quantity = 100;
    request.orderType = OrderType::Limit;
    request.strategyId = 1;
    request.requestTime = nowNanos();

    OrderId orderId = gateway.submitOrder(request);
    EXPECT_NE(orderId, INVALID_ORDER_ID);

    sentMessages.clear();

    // Cancel the order
    bool cancelled = gateway.cancelOrder(orderId);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(sentMessages.size(), 1u);

    // Verify the message is an OrderCancelRequest
    FIX::MsgType msgType;
    sentMessages[0].getHeader().get(msgType);
    EXPECT_EQ(msgType.getValue(), "F");
}

TEST_F(GatewayQuickFixTest, ProcessExecutionReport) {
    // Submit an order first
    OrderRequest request;
    request.symbolId = 1;
    request.side = Side::Buy;
    request.price = priceFromDouble(50000.0);
    request.quantity = 100;
    request.orderType = OrderType::Limit;
    request.strategyId = 1;
    request.requestTime = nowNanos();

    OrderId orderId = gateway.submitOrder(request);

    // Create and process execution report
    FIX44::ExecutionReport report;
    report.set(FIX::ClOrdID(std::to_string(orderId)));
    report.set(FIX::OrderID("EX123"));
    report.set(FIX::ExecID("EXEC001"));
    report.set(FIX::ExecType(qfix::EXEC_TYPE_NEW));
    report.set(FIX::OrdStatus('0'));
    report.set(FIX::Symbol("SYM1"));
    report.set(FIX::Side(qfix::SIDE_BUY));
    report.set(FIX::LeavesQty(100));
    report.set(FIX::CumQty(0));

    gateway.onExecutionReport(report);

    // Verify order state updated
    const InternalOrder* order = gateway.getOrder(orderId);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->status, OrderStatus::New);
}

//==============================================================================
// QuickFIX Stats Tests
//==============================================================================

TEST_F(QuickFixMessageTest, StatsInitialValues) {
    QuickFixStats stats;

    EXPECT_EQ(stats.messagesReceived.load(), 0u);
    EXPECT_EQ(stats.messagesSent.load(), 0u);
    EXPECT_EQ(stats.marketDataMessages.load(), 0u);
    EXPECT_EQ(stats.executionReports.load(), 0u);
    EXPECT_EQ(stats.parseErrors.load(), 0u);
    EXPECT_EQ(stats.sessionResets.load(), 0u);
}

TEST_F(QuickFixMessageTest, StatsReset) {
    QuickFixStats stats;

    stats.messagesReceived.store(100);
    stats.messagesSent.store(50);
    stats.marketDataMessages.store(80);

    stats.reset();

    EXPECT_EQ(stats.messagesReceived.load(), 0u);
    EXPECT_EQ(stats.messagesSent.load(), 0u);
    EXPECT_EQ(stats.marketDataMessages.load(), 0u);
}

/**
 * @file QuickFixApplication.hpp
 * @brief QuickFIX integration for FIX protocol handling.
 *
 * Implements a QuickFIX Application for receiving market data and
 * sending orders via the FIX protocol.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_QUICKFIX_APPLICATION_HPP
#define HFT_NANOTICK_QUICKFIX_APPLICATION_HPP

// QuickFIX support disabled - ARM64 compatibility issues with v1.15.1
// Define HFT_ENABLE_QUICKFIX to enable FIX protocol support
#ifdef HFT_ENABLE_QUICKFIX

#include "../core/Types.hpp"
#include "../core/Timestamp.hpp"
#include "../core/SPSCQueue.hpp"

#include <quickfix/Application.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/Session.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>
#include <quickfix/fix44/OrderCancelReplaceRequest.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/MarketDataSnapshotFullRefresh.h>
#include <quickfix/fix44/MarketDataIncrementalRefresh.h>
#include <quickfix/fix44/MarketDataRequest.h>

#include <functional>
#include <atomic>
#include <mutex>

namespace hft {

//==============================================================================
// FIX Protocol Constants (QuickFIX version)
//==============================================================================

namespace qfix {

/// FIX 4.4 Side values
inline constexpr char SIDE_BUY = '1';
inline constexpr char SIDE_SELL = '2';

/// MD Entry types
inline constexpr char MD_ENTRY_BID = '0';
inline constexpr char MD_ENTRY_ASK = '1';
inline constexpr char MD_ENTRY_TRADE = '2';

/// MD Update actions
inline constexpr char MD_ACTION_NEW = '0';
inline constexpr char MD_ACTION_CHANGE = '1';
inline constexpr char MD_ACTION_DELETE = '2';

/// Order types
inline constexpr char ORD_TYPE_MARKET = '1';
inline constexpr char ORD_TYPE_LIMIT = '2';

/// Time in Force
inline constexpr char TIF_DAY = '0';
inline constexpr char TIF_GTC = '1';
inline constexpr char TIF_IOC = '3';
inline constexpr char TIF_FOK = '4';

/// Exec types
inline constexpr char EXEC_TYPE_NEW = '0';
inline constexpr char EXEC_TYPE_PARTIAL_FILL = '1';
inline constexpr char EXEC_TYPE_FILL = '2';
inline constexpr char EXEC_TYPE_CANCELED = '4';
inline constexpr char EXEC_TYPE_REPLACED = '5';
inline constexpr char EXEC_TYPE_REJECTED = '8';

} // namespace qfix

//==============================================================================
// Market Data Entry (from QuickFIX message)
//==============================================================================

/**
 * @brief Normalized market data entry extracted from QuickFIX message.
 */
struct QuickFixMdEntry {
    char entryType{qfix::MD_ENTRY_TRADE}; ///< '0'=Bid, '1'=Ask, '2'=Trade
    char updateAction{qfix::MD_ACTION_NEW}; ///< '0'=New, '1'=Change, '2'=Delete
    Price price{INVALID_PRICE}; ///< Price
    Quantity size{0}; ///< Size
    std::string entryId; ///< Entry ID (if provided)
    Timestamp receiveTime{0}; ///< Local receive timestamp
};

//==============================================================================
// Execution Report (from QuickFIX message)
//==============================================================================

/**
 * @brief Normalized execution report extracted from QuickFIX message.
 */
struct QuickFixExecReport {
    OrderId clOrdId{INVALID_ORDER_ID}; ///< Client order ID
    OrderId orderId{INVALID_ORDER_ID}; ///< Exchange order ID
    char execType{qfix::EXEC_TYPE_REJECTED}; ///< Execution type
    OrderStatus status{OrderStatus::Rejected}; ///< Order status
    Price lastPx{INVALID_PRICE}; ///< Last fill price
    Quantity lastQty{0}; ///< Last fill quantity
    Quantity cumQty{0}; ///< Cumulative filled quantity
    Quantity leavesQty{0}; ///< Remaining quantity
    std::string text; ///< Reject reason text
    Timestamp receiveTime{0}; ///< Local receive timestamp
};

//==============================================================================
// QuickFIX Application Statistics
//==============================================================================

/**
 * @brief Statistics for QuickFIX application.
 */
struct alignas(CACHE_LINE_SIZE) QuickFixStats {
    std::atomic<std::uint64_t> messagesReceived{0};
    std::atomic<std::uint64_t> messagesSent{0};
    std::atomic<std::uint64_t> marketDataMessages{0};
    std::atomic<std::uint64_t> executionReports{0};
    std::atomic<std::uint64_t> parseErrors{0};
    std::atomic<std::uint64_t> sessionResets{0};

    void reset() noexcept {
        messagesReceived.store(0, std::memory_order_relaxed);
        messagesSent.store(0, std::memory_order_relaxed);
        marketDataMessages.store(0, std::memory_order_relaxed);
        executionReports.store(0, std::memory_order_relaxed);
        parseErrors.store(0, std::memory_order_relaxed);
        sessionResets.store(0, std::memory_order_relaxed);
    }
};

//==============================================================================
// QuickFIX Application
//==============================================================================

/**
 * @brief QuickFIX Application implementation for HFT.
 *
 * Handles FIX protocol communication using the QuickFIX library.
 * Integrates with SPSC queues for low-latency message passing.
 *
 * @tparam MdQueueSize Size of market data queue.
 * @tparam ExecQueueSize Size of execution report queue.
 */
template <std::size_t MdQueueSize = 65536, std::size_t ExecQueueSize = 4096>
class QuickFixApplication : public FIX::Application, public FIX::MessageCracker {
public:
    using MarketDataCallback = std::function<void(const QuickFixMdEntry&)>;
    using ExecutionCallback = std::function<void(const QuickFixExecReport&)>;

    QuickFixApplication() = default;
    ~QuickFixApplication() override = default;

    // Non-copyable
    QuickFixApplication(const QuickFixApplication&) = delete;
    QuickFixApplication& operator=(const QuickFixApplication&) = delete;

    //==========================================================================
    // FIX::Application Interface
    //==========================================================================

    void onCreate(const FIX::SessionID& sessionId) override {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        m_sessionId = sessionId;
    }

    void onLogon(const FIX::SessionID& /*sessionId*/) override { m_connected.store(true, std::memory_order_release); }

    void onLogout(const FIX::SessionID& /*sessionId*/) override { m_connected.store(false, std::memory_order_release); }

    void toAdmin(FIX::Message& /*message*/, const FIX::SessionID& /*sessionId*/) override {
        // Can customize admin messages here (e.g., add password to Logon)
    }

    void toApp(FIX::Message& /*message*/, const FIX::SessionID& /*sessionId*/) override {
        m_stats.messagesSent.fetch_add(1, std::memory_order_relaxed);
    }

    void fromAdmin(const FIX::Message& /*message*/, const FIX::SessionID& /*sessionId*/) override {
        m_stats.messagesReceived.fetch_add(1, std::memory_order_relaxed);
    }

    void fromApp(const FIX::Message& message, const FIX::SessionID& sessionId) override {
        m_stats.messagesReceived.fetch_add(1, std::memory_order_relaxed);
        crack(message, sessionId);
    }

    //==========================================================================
    // FIX::MessageCracker Handlers
    //==========================================================================

    void onMessage(const FIX44::MarketDataSnapshotFullRefresh& message, const FIX::SessionID& /*sessionId*/) override {
        processMarketDataSnapshot(message);
    }

    void onMessage(const FIX44::MarketDataIncrementalRefresh& message, const FIX::SessionID& /*sessionId*/) override {
        processMarketDataIncremental(message);
    }

    void onMessage(const FIX44::ExecutionReport& message, const FIX::SessionID& /*sessionId*/) override {
        processExecutionReport(message);
    }

    //==========================================================================
    // Order Submission
    //==========================================================================

    /**
     * @brief Send a new order.
     *
     * @param clOrdId Client order ID.
     * @param symbol Symbol.
     * @param side Side (Buy/Sell).
     * @param price Limit price.
     * @param quantity Order quantity.
     * @param orderType Order type ('1'=Market, '2'=Limit).
     * @return true if message was sent.
     */
    bool sendNewOrder(OrderId clOrdId, const std::string& symbol, Side side, Price price, Quantity quantity,
                      char orderType = qfix::ORD_TYPE_LIMIT) {
        if (!m_connected.load(std::memory_order_acquire)) {
            return false;
        }

        FIX44::NewOrderSingle order;

        order.set(FIX::ClOrdID(std::to_string(clOrdId)));
        order.set(FIX::Symbol(symbol));
        order.set(FIX::Side(side == Side::Buy ? qfix::SIDE_BUY : qfix::SIDE_SELL));
        order.set(FIX::TransactTime(FIX::UtcTimeStamp()));
        order.set(FIX::OrdType(orderType));

        if (orderType == qfix::ORD_TYPE_LIMIT) {
            order.set(FIX::Price(priceToDouble(price)));
        }

        order.set(FIX::OrderQty(static_cast<double>(quantity)));
        order.set(FIX::TimeInForce(qfix::TIF_DAY));

        return sendMessage(order);
    }

    /**
     * @brief Send an order cancel request.
     *
     * @param clOrdId Client order ID.
     * @param origClOrdId Original client order ID to cancel.
     * @param symbol Symbol.
     * @param side Side.
     * @return true if message was sent.
     */
    bool sendCancelOrder(OrderId clOrdId, OrderId origClOrdId, const std::string& symbol, Side side) {
        if (!m_connected.load(std::memory_order_acquire)) {
            return false;
        }

        FIX44::OrderCancelRequest cancel;

        cancel.set(FIX::ClOrdID(std::to_string(clOrdId)));
        cancel.set(FIX::OrigClOrdID(std::to_string(origClOrdId)));
        cancel.set(FIX::Symbol(symbol));
        cancel.set(FIX::Side(side == Side::Buy ? qfix::SIDE_BUY : qfix::SIDE_SELL));
        cancel.set(FIX::TransactTime(FIX::UtcTimeStamp()));

        return sendMessage(cancel);
    }

    /**
     * @brief Send an order replace/modify request.
     *
     * @param clOrdId New client order ID.
     * @param origClOrdId Original client order ID to modify.
     * @param symbol Symbol.
     * @param side Side.
     * @param price New price.
     * @param quantity New quantity.
     * @return true if message was sent.
     */
    bool sendReplaceOrder(OrderId clOrdId, OrderId origClOrdId, const std::string& symbol, Side side, Price price,
                          Quantity quantity) {
        if (!m_connected.load(std::memory_order_acquire)) {
            return false;
        }

        FIX44::OrderCancelReplaceRequest replace;

        replace.set(FIX::ClOrdID(std::to_string(clOrdId)));
        replace.set(FIX::OrigClOrdID(std::to_string(origClOrdId)));
        replace.set(FIX::Symbol(symbol));
        replace.set(FIX::Side(side == Side::Buy ? qfix::SIDE_BUY : qfix::SIDE_SELL));
        replace.set(FIX::TransactTime(FIX::UtcTimeStamp()));
        replace.set(FIX::OrdType(qfix::ORD_TYPE_LIMIT));
        replace.set(FIX::Price(priceToDouble(price)));
        replace.set(FIX::OrderQty(static_cast<double>(quantity)));

        return sendMessage(replace);
    }

    /**
     * @brief Subscribe to market data.
     *
     * @param mdReqId Request ID.
     * @param symbol Symbol to subscribe to.
     * @param subscriptionType '1'=Snapshot+Updates, '2'=Unsubscribe.
     * @return true if message was sent.
     */
    bool subscribeMarketData(const std::string& mdReqId, const std::string& symbol, char subscriptionType = '1') {
        if (!m_connected.load(std::memory_order_acquire)) {
            return false;
        }

        FIX44::MarketDataRequest request;

        request.set(FIX::MDReqID(mdReqId));
        request.set(FIX::SubscriptionRequestType(subscriptionType));
        request.set(FIX::MarketDepth(0)); // Full book

        // Add MD entry types
        FIX44::MarketDataRequest::NoMDEntryTypes entryTypesGroup;
        entryTypesGroup.set(FIX::MDEntryType(qfix::MD_ENTRY_BID));
        request.addGroup(entryTypesGroup);
        entryTypesGroup.set(FIX::MDEntryType(qfix::MD_ENTRY_ASK));
        request.addGroup(entryTypesGroup);
        entryTypesGroup.set(FIX::MDEntryType(qfix::MD_ENTRY_TRADE));
        request.addGroup(entryTypesGroup);

        // Add symbol
        FIX44::MarketDataRequest::NoRelatedSym symbolGroup;
        symbolGroup.set(FIX::Symbol(symbol));
        request.addGroup(symbolGroup);

        return sendMessage(request);
    }

    //==========================================================================
    // Queue Access
    //==========================================================================

    /**
     * @brief Try to pop a market data entry from the queue.
     *
     * @param entry Output entry.
     * @return true if entry was popped.
     */
    [[nodiscard]] bool tryPopMarketData(QuickFixMdEntry& entry) noexcept { return m_mdQueue.tryPop(entry); }

    /**
     * @brief Try to pop an execution report from the queue.
     *
     * @param report Output report.
     * @return true if report was popped.
     */
    [[nodiscard]] bool tryPopExecutionReport(QuickFixExecReport& report) noexcept { return m_execQueue.tryPop(report); }

    /**
     * @brief Get approximate market data queue depth.
     */
    [[nodiscard]] std::size_t mdQueueDepth() const noexcept { return m_mdQueue.sizeApprox(); }

    /**
     * @brief Get approximate execution report queue depth.
     */
    [[nodiscard]] std::size_t execQueueDepth() const noexcept { return m_execQueue.sizeApprox(); }

    //==========================================================================
    // Callbacks (alternative to queue-based processing)
    //==========================================================================

    void setMarketDataCallback(MarketDataCallback callback) { m_mdCallback = std::move(callback); }

    void setExecutionCallback(ExecutionCallback callback) { m_execCallback = std::move(callback); }

    //==========================================================================
    // Status
    //==========================================================================

    [[nodiscard]] bool isConnected() const noexcept { return m_connected.load(std::memory_order_acquire); }

    [[nodiscard]] const QuickFixStats& stats() const noexcept { return m_stats; }

    void resetStats() noexcept { m_stats.reset(); }

private:
    void processMarketDataSnapshot(const FIX44::MarketDataSnapshotFullRefresh& message) {
        m_stats.marketDataMessages.fetch_add(1, std::memory_order_relaxed);

        Timestamp receiveTime = nowNanos();

        // Extract symbol
        FIX::Symbol symbol;
        message.get(symbol);

        // Process MD entries
        int numEntries = static_cast<int>(message.groupCount(FIX::FIELD::NoMDEntries));

        for (int i = 1; i <= numEntries; ++i) {
            FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
            message.getGroup(static_cast<unsigned int>(i), group);

            QuickFixMdEntry entry;
            entry.receiveTime = receiveTime;
            entry.updateAction = qfix::MD_ACTION_NEW; // Snapshot implies new

            FIX::MDEntryType entryType;
            group.get(entryType);
            entry.entryType = entryType.getValue();

            FIX::MDEntryPx px;
            if (group.isSet(px)) {
                group.get(px);
                entry.price = priceFromDouble(px.getValue());
            }

            FIX::MDEntrySize size;
            if (group.isSet(size)) {
                group.get(size);
                entry.size = static_cast<Quantity>(size.getValue());
            }

            FIX::MDEntryID entryId;
            if (group.isSet(entryId)) {
                group.get(entryId);
                entry.entryId = entryId.getValue();
            }

            enqueueMarketData(entry);
        }
    }

    void processMarketDataIncremental(const FIX44::MarketDataIncrementalRefresh& message) {
        m_stats.marketDataMessages.fetch_add(1, std::memory_order_relaxed);

        Timestamp receiveTime = nowNanos();

        // Process MD entries
        int numEntries = static_cast<int>(message.groupCount(FIX::FIELD::NoMDEntries));

        for (int i = 1; i <= numEntries; ++i) {
            FIX44::MarketDataIncrementalRefresh::NoMDEntries group;
            message.getGroup(static_cast<unsigned int>(i), group);

            QuickFixMdEntry entry;
            entry.receiveTime = receiveTime;

            FIX::MDUpdateAction action;
            if (group.isSet(action)) {
                group.get(action);
                entry.updateAction = action.getValue();
            }

            FIX::MDEntryType entryType;
            group.get(entryType);
            entry.entryType = entryType.getValue();

            FIX::MDEntryPx px;
            if (group.isSet(px)) {
                group.get(px);
                entry.price = priceFromDouble(px.getValue());
            }

            FIX::MDEntrySize size;
            if (group.isSet(size)) {
                group.get(size);
                entry.size = static_cast<Quantity>(size.getValue());
            }

            FIX::MDEntryID entryId;
            if (group.isSet(entryId)) {
                group.get(entryId);
                entry.entryId = entryId.getValue();
            }

            enqueueMarketData(entry);
        }
    }

    void processExecutionReport(const FIX44::ExecutionReport& message) {
        m_stats.executionReports.fetch_add(1, std::memory_order_relaxed);

        QuickFixExecReport report;
        report.receiveTime = nowNanos();

        FIX::ClOrdID clOrdId;
        message.get(clOrdId);
        report.clOrdId = static_cast<OrderId>(std::stoull(clOrdId.getValue()));

        FIX::OrderID orderId;
        if (message.isSet(orderId)) {
            message.get(orderId);
            report.orderId = static_cast<OrderId>(std::stoull(orderId.getValue()));
        }

        FIX::ExecType execType;
        message.get(execType);
        report.execType = execType.getValue();

        // Map exec type to order status
        switch (report.execType) {
            case qfix::EXEC_TYPE_NEW: report.status = OrderStatus::New; break;
            case qfix::EXEC_TYPE_PARTIAL_FILL: report.status = OrderStatus::PartiallyFilled; break;
            case qfix::EXEC_TYPE_FILL: report.status = OrderStatus::Filled; break;
            case qfix::EXEC_TYPE_CANCELED: report.status = OrderStatus::Canceled; break;
            case qfix::EXEC_TYPE_REJECTED: report.status = OrderStatus::Rejected; break;
            default: report.status = OrderStatus::New; break;
        }

        FIX::LastPx lastPx;
        if (message.isSet(lastPx)) {
            message.get(lastPx);
            report.lastPx = priceFromDouble(lastPx.getValue());
        }

        FIX::LastQty lastQty;
        if (message.isSet(lastQty)) {
            message.get(lastQty);
            report.lastQty = static_cast<Quantity>(lastQty.getValue());
        }

        FIX::CumQty cumQty;
        if (message.isSet(cumQty)) {
            message.get(cumQty);
            report.cumQty = static_cast<Quantity>(cumQty.getValue());
        }

        FIX::LeavesQty leavesQty;
        if (message.isSet(leavesQty)) {
            message.get(leavesQty);
            report.leavesQty = static_cast<Quantity>(leavesQty.getValue());
        }

        FIX::Text text;
        if (message.isSet(text)) {
            message.get(text);
            report.text = text.getValue();
        }

        enqueueExecutionReport(report);
    }

    void enqueueMarketData(const QuickFixMdEntry& entry) {
        if (m_mdCallback) {
            m_mdCallback(entry);
        }
        (void)m_mdQueue.tryPush(entry); // Best effort
    }

    void enqueueExecutionReport(const QuickFixExecReport& report) {
        if (m_execCallback) {
            m_execCallback(report);
        }
        (void)m_execQueue.tryPush(report); // Best effort
    }

    bool sendMessage(FIX::Message& message) {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        return FIX::Session::sendToTarget(message, m_sessionId);
    }

    // Session state
    FIX::SessionID m_sessionId;
    std::mutex m_sessionMutex;
    std::atomic<bool> m_connected{false};

    // Message queues
    SPSCQueue<QuickFixMdEntry, MdQueueSize> m_mdQueue;
    SPSCQueue<QuickFixExecReport, ExecQueueSize> m_execQueue;

    // Optional callbacks
    MarketDataCallback m_mdCallback;
    ExecutionCallback m_execCallback;

    // Statistics
    QuickFixStats m_stats;
};

//==============================================================================
// Type Conversion Utilities
//==============================================================================

/**
 * @brief Convert QuickFIX UtcTimeStamp to nanoseconds timestamp.
 */
[[nodiscard]] inline Timestamp utcTimestampToNanos(const FIX::UtcTimeStamp& ts) {
    auto seconds = static_cast<Timestamp>(ts.getTimeT());
    auto millis = static_cast<Timestamp>(ts.getMillisecond());
    return seconds * 1'000'000'000LL + millis * 1'000'000LL;
}

/**
 * @brief Convert nanoseconds timestamp to QuickFIX UtcTimeStamp.
 */
[[nodiscard]] inline FIX::UtcTimeStamp nanosToUtcTimestamp(Timestamp nanos) {
    time_t seconds = static_cast<time_t>(nanos / 1'000'000'000LL);
    int millis = static_cast<int>((nanos / 1'000'000LL) % 1000);
    return FIX::UtcTimeStamp(seconds, millis);
}

/// Default QuickFIX application type
using DefaultQuickFixApp = QuickFixApplication<65536, 4096>;

} // namespace hft

#endif // HFT_ENABLE_QUICKFIX

#endif // HFT_NANOTICK_QUICKFIX_APPLICATION_HPP

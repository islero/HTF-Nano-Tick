/**
 * @file OrderEntryGateway.hpp
 * @brief Order entry gateway for order submission and execution management.
 *
 * Handles the order lifecycle from strategy signal to exchange submission:
 * - Order submission with rate limiting
 * - Order tracking and state management
 * - Execution report processing
 * - Risk checks (pre-trade validation)
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_ORDER_ENTRY_GATEWAY_HPP
#define HFT_NANOTICK_ORDER_ENTRY_GATEWAY_HPP

#include "../core/Types.hpp"
#include "../core/SPSCQueue.hpp"
#include "../core/Timestamp.hpp"
#ifdef HFT_ENABLE_QUICKFIX
#include "../market_data/QuickFixApplication.hpp"
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>
#include <quickfix/fix44/OrderCancelReplaceRequest.h>
#include <quickfix/fix44/ExecutionReport.h>
#endif
#include "../strategy/StrategyEngine.hpp"

#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace hft {

//==============================================================================
// Order Gateway Configuration
//==============================================================================

/**
 * @brief Configuration for the order entry gateway.
 */
struct GatewayConfig {
    /// Maximum orders per second (rate limit)
    std::uint32_t maxOrdersPerSecond{1000};

    /// Maximum pending orders (not yet acknowledged)
    std::size_t maxPendingOrders{100};

    /// Maximum total open orders
    std::size_t maxOpenOrders{1000};

    /// Order timeout in nanoseconds
    Timestamp orderTimeoutNanos{5'000'000'000};  // 5 seconds

    /// Enable pre-trade risk checks
    bool enableRiskChecks{true};

    /// Maximum order value (price * quantity)
    std::int64_t maxOrderValue{1'000'000'000'000};  // $10,000 at 8 decimals

    /// Maximum position per symbol
    Quantity maxPositionPerSymbol{100'000};

    /// Sender CompID for FIX messages
    std::string senderCompId{"SENDER"};

    /// Target CompID for FIX messages
    std::string targetCompId{"TARGET"};
};

//==============================================================================
// Internal Order State
//==============================================================================

/**
 * @brief Internal order representation with full state tracking.
 */
struct alignas(CACHE_LINE_SIZE) InternalOrder {
    OrderId     clientOrderId;    ///< Client-assigned order ID
    OrderId     exchangeOrderId;  ///< Exchange-assigned order ID
    SymbolId    symbolId;         ///< Instrument
    std::string symbol;           ///< Symbol string
    Side        side;             ///< Buy/Sell
    Price       price;            ///< Limit price
    Quantity    orderQty;         ///< Original quantity
    Quantity    filledQty;        ///< Cumulative filled quantity
    Quantity    remainingQty;     ///< Remaining quantity
    OrderStatus status;           ///< Current status
    OrderType   orderType;        ///< Order type
    Timestamp   submitTime;       ///< When order was submitted
    Timestamp   lastUpdateTime;   ///< Last status change
    std::uint64_t strategyId;     ///< Originating strategy

    InternalOrder() noexcept = default;

    [[nodiscard]] bool isActive() const noexcept {
        return status == OrderStatus::New ||
               status == OrderStatus::PartiallyFilled ||
               status == OrderStatus::PendingNew ||
               status == OrderStatus::PendingCancel;
    }

    [[nodiscard]] bool isTerminal() const noexcept {
        return status == OrderStatus::Filled ||
               status == OrderStatus::Canceled ||
               status == OrderStatus::Rejected;
    }
};

//==============================================================================
// Execution Report
//==============================================================================

/**
 * @brief Execution report from exchange.
 */
struct ExecutionReport {
    OrderId     clientOrderId;
    OrderId     exchangeOrderId;
    OrderStatus status;
    Price       lastPrice;        ///< Last fill price
    Quantity    lastQty;          ///< Last fill quantity
    Quantity    cumulativeQty;    ///< Total filled quantity
    Quantity    leavesQty;        ///< Remaining quantity
    Timestamp   transactTime;     ///< Exchange timestamp
    int         rejectReason;     ///< Reason code if rejected
    std::string text;             ///< Reject reason text
};

//==============================================================================
// Native Outbound Order Message
//==============================================================================

/**
 * @brief Protocol-neutral outbound order command.
 *
 * Gateways can translate this object to FIX, OUCH, binary native protocols, or a
 * simulator without making the core order manager depend on a transport library.
 */
enum class OutboundOrderAction : std::uint8_t {
    New = 0,
    Cancel = 1,
    Replace = 2
};

struct OutboundOrderMessage {
    OutboundOrderAction action{OutboundOrderAction::New};
    OrderId clientOrderId{INVALID_ORDER_ID};
    OrderId origClientOrderId{INVALID_ORDER_ID};
    OrderId exchangeOrderId{INVALID_ORDER_ID};
    SymbolId symbolId{0};
    std::string symbol;
    Side side{Side::Buy};
    Price price{INVALID_PRICE};
    Quantity quantity{0};
    OrderType orderType{OrderType::Limit};
    Timestamp transactTime{0};
};

//==============================================================================
// Gateway Statistics
//==============================================================================

/**
 * @brief Gateway performance and health statistics.
 */
struct alignas(CACHE_LINE_SIZE) GatewayStats {
    std::atomic<std::uint64_t> ordersSubmitted{0};
    std::atomic<std::uint64_t> ordersAcknowledged{0};
    std::atomic<std::uint64_t> ordersFilled{0};
    std::atomic<std::uint64_t> ordersRejected{0};
    std::atomic<std::uint64_t> ordersCanceled{0};
    std::atomic<std::uint64_t> riskRejections{0};
    std::atomic<std::uint64_t> rateLimitRejections{0};
    std::atomic<std::int64_t> minSubmitLatencyNanos{std::numeric_limits<std::int64_t>::max()};
    std::atomic<std::int64_t> maxSubmitLatencyNanos{0};

    void reset() noexcept {
        ordersSubmitted.store(0, std::memory_order_relaxed);
        ordersAcknowledged.store(0, std::memory_order_relaxed);
        ordersFilled.store(0, std::memory_order_relaxed);
        ordersRejected.store(0, std::memory_order_relaxed);
        ordersCanceled.store(0, std::memory_order_relaxed);
        riskRejections.store(0, std::memory_order_relaxed);
        rateLimitRejections.store(0, std::memory_order_relaxed);
        minSubmitLatencyNanos.store(std::numeric_limits<std::int64_t>::max(), std::memory_order_relaxed);
        maxSubmitLatencyNanos.store(0, std::memory_order_relaxed);
    }
};

//==============================================================================
// Order Entry Gateway
//==============================================================================

/**
 * @brief Order entry gateway with risk management and rate limiting.
 *
 * Manages the complete order lifecycle including submission, tracking,
 * and execution reporting. Implements pre-trade risk checks and
 * rate limiting to prevent runaway algorithms.
 *
 * @tparam QueueCapacity Size of internal order queue.
 */
template <std::size_t QueueCapacity = 4096>
class OrderEntryGateway {
public:
    static_assert(QueueCapacity > 0, "QueueCapacity must be greater than zero");

    /// Callback for order status updates
    using OrderCallback = std::function<void(const InternalOrder&)>;

    /// Callback for sending protocol-neutral outbound order messages
    using SendCallback = std::function<bool(const OutboundOrderMessage&)>;

    /**
     * @brief Construct order entry gateway.
     * @param config Gateway configuration.
     */
    explicit OrderEntryGateway(const GatewayConfig& config = {}) noexcept
        : m_config(config)
        , m_nextOrderId(1)
    {}

    ~OrderEntryGateway() = default;

    // Non-copyable
    OrderEntryGateway(const OrderEntryGateway&) = delete;
    OrderEntryGateway& operator=(const OrderEntryGateway&) = delete;

    //==========================================================================
    // Order Submission
    //==========================================================================

    /**
     * @brief Submit a new order.
     *
     * Performs risk checks, rate limiting, and queues the order for
     * transmission to the exchange.
     *
     * @param request Order request from strategy.
     * @return Assigned client order ID, or INVALID_ORDER_ID on rejection.
     */
    [[nodiscard]] OrderId submitOrder(const OrderRequest& request) noexcept {
        Timestamp now = nowNanos();

        //======================================================================
        // Rate Limiting
        //======================================================================
        if (!checkRateLimit(now)) [[unlikely]] {
            m_stats.rateLimitRejections.fetch_add(1, std::memory_order_relaxed);
            return INVALID_ORDER_ID;
        }

        //======================================================================
        // Risk Checks
        //======================================================================
        if (m_config.enableRiskChecks && !passesRiskChecks(request)) [[unlikely]] {
            m_stats.riskRejections.fetch_add(1, std::memory_order_relaxed);
            return INVALID_ORDER_ID;
        }

        //======================================================================
        // Create Internal Order
        //======================================================================
        OrderId orderId = m_nextOrderId.fetch_add(1, std::memory_order_relaxed);

        InternalOrder order;
        order.clientOrderId = orderId;
        order.exchangeOrderId = INVALID_ORDER_ID;
        order.symbolId = request.symbolId;
        order.symbol = symbolIdToString(request.symbolId);
        order.side = request.side;
        order.price = request.price;
        order.orderQty = request.quantity;
        order.filledQty = 0;
        order.remainingQty = request.quantity;
        order.status = OrderStatus::PendingNew;
        order.orderType = request.orderType;
        order.submitTime = now;
        order.lastUpdateTime = now;
        order.strategyId = request.strategyId;

        // Store order
        m_orders[orderId] = order;
        m_pendingOrders.insert(orderId);

        // Update position tracking
        updatePositionOnOrder(request);

        if (m_sendCallback) {
            OutboundOrderMessage message;
            message.action = OutboundOrderAction::New;
            message.clientOrderId = orderId;
            message.symbolId = request.symbolId;
            message.symbol = order.symbol;
            message.side = request.side;
            message.price = request.price;
            message.quantity = request.quantity;
            message.orderType = request.orderType;
            message.transactTime = now;
            (void)m_sendCallback(message);
        }

        m_stats.ordersSubmitted.fetch_add(1, std::memory_order_relaxed);

        // Latency tracking
        Timestamp latency = nowNanos() - request.requestTime;
        updateLatencyStats(latency);

        return orderId;
    }

    /**
     * @brief Cancel an existing order.
     *
     * @param orderId Client order ID to cancel.
     * @return true if cancel request was sent.
     */
    [[nodiscard]] bool cancelOrder(OrderId orderId) noexcept {
        auto it = m_orders.find(orderId);
        if (it == m_orders.end()) [[unlikely]] {
            return false;
        }

        InternalOrder& order = it->second;
        if (!order.isActive()) {
            return false;
        }

        order.status = OrderStatus::PendingCancel;
        order.lastUpdateTime = nowNanos();

        if (m_sendCallback) {
            OrderId cancelId = m_nextOrderId.fetch_add(1, std::memory_order_relaxed);

            OutboundOrderMessage message;
            message.action = OutboundOrderAction::Cancel;
            message.clientOrderId = cancelId;
            message.origClientOrderId = orderId;
            message.exchangeOrderId = order.exchangeOrderId;
            message.symbolId = order.symbolId;
            message.symbol = order.symbol;
            message.side = order.side;
            message.transactTime = order.lastUpdateTime;
            (void)m_sendCallback(message);
        }

        return true;
    }

    /**
     * @brief Modify/replace an existing order.
     *
     * @param orderId Client order ID to modify.
     * @param newPrice New price (0 to keep unchanged).
     * @param newQty New quantity (0 to keep unchanged).
     * @return New client order ID, or INVALID_ORDER_ID on failure.
     */
    [[nodiscard]] OrderId replaceOrder(OrderId orderId, Price newPrice, Quantity newQty) noexcept {
        auto it = m_orders.find(orderId);
        if (it == m_orders.end()) [[unlikely]] {
            return INVALID_ORDER_ID;
        }

        InternalOrder& order = it->second;
        if (!order.isActive()) {
            return INVALID_ORDER_ID;
        }

        OrderId newOrderId = m_nextOrderId.fetch_add(1, std::memory_order_relaxed);

        if (m_sendCallback) {
            OutboundOrderMessage message;
            message.action = OutboundOrderAction::Replace;
            message.clientOrderId = newOrderId;
            message.origClientOrderId = orderId;
            message.exchangeOrderId = order.exchangeOrderId;
            message.symbolId = order.symbolId;
            message.symbol = order.symbol;
            message.side = order.side;
            message.price = newPrice > 0 ? newPrice : order.price;
            message.quantity = newQty > 0 ? newQty : order.orderQty;
            message.orderType = order.orderType;
            message.transactTime = nowNanos();
            (void)m_sendCallback(message);
        }

        return newOrderId;
    }

    /**
     * @brief Cancel all orders for a symbol.
     *
     * @param symbolId Symbol to cancel orders for.
     * @return Number of orders canceled.
     */
    std::size_t cancelAllOrders(SymbolId symbolId) noexcept {
        std::size_t count = 0;
        for (auto& [id, order] : m_orders) {
            if (order.symbolId == symbolId && order.isActive()) {
                if (cancelOrder(id)) {
                    ++count;
                }
            }
        }
        return count;
    }

    //==========================================================================
    // Execution Report Processing
    //==========================================================================

    /**
     * @brief Process an execution report from the exchange.
     *
     * @param report Execution report.
     */
    void onExecutionReport(const ExecutionReport& report) noexcept {
        auto it = m_orders.find(report.clientOrderId);
        if (it == m_orders.end()) [[unlikely]] {
            return;
        }

        InternalOrder& order = it->second;

        // Update order state
        order.exchangeOrderId = report.exchangeOrderId;
        order.status = report.status;
        order.filledQty = report.cumulativeQty;
        order.remainingQty = report.leavesQty;
        order.lastUpdateTime = nowNanos();

        // Handle state transitions
        switch (report.status) {
            case OrderStatus::New:
                m_pendingOrders.erase(report.clientOrderId);
                m_stats.ordersAcknowledged.fetch_add(1, std::memory_order_relaxed);
                break;

            case OrderStatus::Filled:
            case OrderStatus::PartiallyFilled:
                if (report.lastQty > 0) {
                    m_stats.ordersFilled.fetch_add(1, std::memory_order_relaxed);
                    updatePositionOnFill(order, report.lastQty);
                }
                if (report.status == OrderStatus::Filled) {
                    cleanupOrder(report.clientOrderId);
                }
                break;

            case OrderStatus::Canceled:
                m_stats.ordersCanceled.fetch_add(1, std::memory_order_relaxed);
                updatePositionOnCancel(order);
                cleanupOrder(report.clientOrderId);
                break;

            case OrderStatus::Rejected:
                m_stats.ordersRejected.fetch_add(1, std::memory_order_relaxed);
                updatePositionOnCancel(order);
                cleanupOrder(report.clientOrderId);
                break;

            default:
                break;
        }

        // Invoke callback
        if (m_orderCallback) {
            m_orderCallback(order);
        }
    }

#ifdef HFT_ENABLE_QUICKFIX
    /**
     * @brief Process a QuickFIX execution report message.
     */
    void onExecutionReport(const FIX44::ExecutionReport& message) noexcept {
        ExecutionReport report;

        // Extract client order ID
        FIX::ClOrdID clOrdId;
        message.get(clOrdId);
        report.clientOrderId = static_cast<OrderId>(std::stoull(clOrdId.getValue()));

        // Extract exchange order ID
        FIX::OrderID orderId;
        if (message.isSet(orderId)) {
            message.get(orderId);
            report.exchangeOrderId = static_cast<OrderId>(std::stoull(orderId.getValue()));
        }

        // Parse exec type to status
        FIX::ExecType execType;
        message.get(execType);
        char et = execType.getValue();

        switch (et) {
            case qfix::EXEC_TYPE_NEW:
                report.status = OrderStatus::New;
                break;
            case qfix::EXEC_TYPE_PARTIAL_FILL:
                report.status = OrderStatus::PartiallyFilled;
                break;
            case qfix::EXEC_TYPE_FILL:
                report.status = OrderStatus::Filled;
                break;
            case qfix::EXEC_TYPE_CANCELED:
                report.status = OrderStatus::Canceled;
                break;
            case qfix::EXEC_TYPE_REJECTED:
                report.status = OrderStatus::Rejected;
                break;
            default:
                report.status = OrderStatus::New;
                break;
        }

        // Extract fill information
        FIX::LastPx lastPx;
        if (message.isSet(lastPx)) {
            message.get(lastPx);
            report.lastPrice = priceFromDouble(lastPx.getValue());
        }

        FIX::LastQty lastQty;
        if (message.isSet(lastQty)) {
            message.get(lastQty);
            report.lastQty = static_cast<Quantity>(lastQty.getValue());
        }

        FIX::CumQty cumQty;
        if (message.isSet(cumQty)) {
            message.get(cumQty);
            report.cumulativeQty = static_cast<Quantity>(cumQty.getValue());
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

        onExecutionReport(report);
    }

    /**
     * @brief Process a QuickFixExecReport from the application queue.
     */
    void onExecutionReport(const QuickFixExecReport& qfReport) noexcept {
        ExecutionReport report;
        report.clientOrderId = qfReport.clOrdId;
        report.exchangeOrderId = qfReport.orderId;
        report.status = qfReport.status;
        report.lastPrice = qfReport.lastPx;
        report.lastQty = qfReport.lastQty;
        report.cumulativeQty = qfReport.cumQty;
        report.leavesQty = qfReport.leavesQty;
        report.text = qfReport.text;

        onExecutionReport(report);
    }
#endif

    //==========================================================================
    // Order Queries
    //==========================================================================

    /**
     * @brief Get an order by ID.
     * @param orderId Client order ID.
     * @return Pointer to order or nullptr.
     */
    [[nodiscard]] const InternalOrder* getOrder(OrderId orderId) const noexcept {
        auto it = m_orders.find(orderId);
        return (it != m_orders.end()) ? &it->second : nullptr;
    }

    /**
     * @brief Get number of open orders.
     * @return Count of active orders.
     */
    [[nodiscard]] std::size_t openOrderCount() const noexcept {
        std::size_t count = 0;
        for (const auto& [id, order] : m_orders) {
            if (order.isActive()) ++count;
        }
        return count;
    }

    /**
     * @brief Get number of pending orders (awaiting ack).
     * @return Count of pending orders.
     */
    [[nodiscard]] std::size_t pendingOrderCount() const noexcept {
        return m_pendingOrders.size();
    }

    /**
     * @brief Get current position for a symbol.
     * @param symbolId Symbol ID.
     * @return Net position (positive = long, negative = short).
     */
    [[nodiscard]] Quantity getPosition(SymbolId symbolId) const noexcept {
        auto it = m_positions.find(symbolId);
        return (it != m_positions.end()) ? it->second : 0;
    }

    //==========================================================================
    // Callbacks
    //==========================================================================

    /**
     * @brief Set callback for order status updates.
     */
    void setOrderCallback(OrderCallback callback) noexcept {
        m_orderCallback = std::move(callback);
    }

    /**
     * @brief Set callback for sending FIX messages.
     */
    void setSendCallback(SendCallback callback) noexcept {
        m_sendCallback = std::move(callback);
    }

    //==========================================================================
    // Statistics
    //==========================================================================

    [[nodiscard]] const GatewayStats& stats() const noexcept {
        return m_stats;
    }

    void resetStats() noexcept {
        m_stats.reset();
    }

private:
    [[nodiscard]] bool checkRateLimit(Timestamp now) noexcept {
        constexpr Timestamp WINDOW_NANOS = 1'000'000'000;  // 1 second

        while (m_orderTimeCount > 0 &&
               (now - m_orderTimes[m_orderTimeHead]) > WINDOW_NANOS) {
            m_orderTimeHead = (m_orderTimeHead + 1U) % QueueCapacity;
            --m_orderTimeCount;
        }

        const std::size_t limit = std::min<std::size_t>(m_config.maxOrdersPerSecond, QueueCapacity);
        if (m_orderTimeCount >= limit) {
            return false;
        }

        const std::size_t tail = (m_orderTimeHead + m_orderTimeCount) % QueueCapacity;
        m_orderTimes[tail] = now;
        ++m_orderTimeCount;
        return true;
    }

    [[nodiscard]] bool passesRiskChecks(const OrderRequest& request) const noexcept {
        if (request.price <= 0 || request.quantity <= 0) {
            return false;
        }

        if (exceedsMaxOrderValue(request.price, request.quantity, m_config.maxOrderValue)) {
            return false;
        }

        if (wouldExceedPositionLimit(request)) {
            return false;
        }

        // Check max pending orders
        if (m_pendingOrders.size() >= m_config.maxPendingOrders) {
            return false;
        }

        // Check max open orders
        if (openOrderCount() >= m_config.maxOpenOrders) {
            return false;
        }

        return true;
    }

    [[nodiscard]] bool exceedsMaxOrderValue(Price price, Quantity quantity,
                                            std::int64_t maxOrderValue) const noexcept {
#if defined(__SIZEOF_INT128__)
        const __int128 notional = static_cast<__int128>(price) * static_cast<__int128>(quantity);
        const __int128 limit = static_cast<__int128>(maxOrderValue) *
                               static_cast<__int128>(PRICE_MULTIPLIER);
        return notional > limit;
#else
        const long double notional = static_cast<long double>(price) *
                                     static_cast<long double>(quantity);
        const long double limit = static_cast<long double>(maxOrderValue) *
                                  static_cast<long double>(PRICE_MULTIPLIER);
        return notional > limit;
#endif
    }

    [[nodiscard]] bool wouldExceedPositionLimit(const OrderRequest& request) const noexcept {
        const Quantity currentPos = getMappedQuantity(m_positions, request.symbolId);
        const Quantity pendingPos = getMappedQuantity(m_pendingPositions, request.symbolId);

#if defined(__SIZEOF_INT128__)
        const __int128 signedQty = request.side == Side::Buy
            ? static_cast<__int128>(request.quantity)
            : -static_cast<__int128>(request.quantity);
        __int128 exposure = static_cast<__int128>(currentPos) +
                            static_cast<__int128>(pendingPos) +
                            signedQty;
        if (exposure < 0) {
            exposure = -exposure;
        }
        return exposure > static_cast<__int128>(m_config.maxPositionPerSymbol);
#else
        const long double signedQty = request.side == Side::Buy
            ? static_cast<long double>(request.quantity)
            : -static_cast<long double>(request.quantity);
        const long double exposure = std::abs(static_cast<long double>(currentPos) +
                                              static_cast<long double>(pendingPos) +
                                              signedQty);
        return exposure > static_cast<long double>(m_config.maxPositionPerSymbol);
#endif
    }

    [[nodiscard]] static Quantity getMappedQuantity(
        const std::unordered_map<SymbolId, Quantity>& map, SymbolId symbolId) noexcept {
        auto it = map.find(symbolId);
        return it != map.end() ? it->second : 0;
    }

    void updatePositionOnOrder(const OrderRequest& request) noexcept {
        // Reserve position for pending order
        Quantity delta = (request.side == Side::Buy) ?
                         static_cast<Quantity>(request.quantity) :
                         -static_cast<Quantity>(request.quantity);
        m_pendingPositions[request.symbolId] += delta;
    }

    void updatePositionOnFill(const InternalOrder& order, Quantity fillQty) noexcept {
        Quantity delta = (order.side == Side::Buy) ?
                         static_cast<Quantity>(fillQty) :
                         -static_cast<Quantity>(fillQty);
        m_positions[order.symbolId] += delta;
        m_pendingPositions[order.symbolId] -= delta;
    }

    void updatePositionOnCancel(const InternalOrder& order) noexcept {
        Quantity delta = (order.side == Side::Buy) ?
                         static_cast<Quantity>(order.remainingQty) :
                         -static_cast<Quantity>(order.remainingQty);
        m_pendingPositions[order.symbolId] -= delta;
    }

    void cleanupOrder(OrderId orderId) noexcept {
        m_pendingOrders.erase(orderId);
        // Note: We keep the order in m_orders for historical queries
        // In production, implement order archival
    }

    void updateLatencyStats(Timestamp latency) noexcept {
        Timestamp currentMin = m_stats.minSubmitLatencyNanos.load(std::memory_order_relaxed);
        while (latency < currentMin &&
               !m_stats.minSubmitLatencyNanos.compare_exchange_weak(currentMin, latency,
                   std::memory_order_relaxed)) {}

        Timestamp currentMax = m_stats.maxSubmitLatencyNanos.load(std::memory_order_relaxed);
        while (latency > currentMax &&
               !m_stats.maxSubmitLatencyNanos.compare_exchange_weak(currentMax, latency,
                   std::memory_order_relaxed)) {}
    }

    [[nodiscard]] std::string symbolIdToString(SymbolId id) const noexcept {
        // In production, use a symbol mapping table
        return "SYM" + std::to_string(id);
    }

    GatewayConfig m_config;
    std::atomic<OrderId> m_nextOrderId;

    // Order tracking
    std::unordered_map<OrderId, InternalOrder> m_orders;
    std::unordered_set<OrderId> m_pendingOrders;

    // Position tracking
    std::unordered_map<SymbolId, Quantity> m_positions;
    std::unordered_map<SymbolId, Quantity> m_pendingPositions;

    // Rate limiting
    std::array<Timestamp, QueueCapacity> m_orderTimes{};
    std::size_t m_orderTimeHead{0};
    std::size_t m_orderTimeCount{0};

    // Callbacks
    OrderCallback m_orderCallback;
    SendCallback m_sendCallback;

    // Statistics
    GatewayStats m_stats;
};

/// Default gateway type
using DefaultOrderGateway = OrderEntryGateway<4096>;

} // namespace hft

#endif // HFT_NANOTICK_ORDER_ENTRY_GATEWAY_HPP

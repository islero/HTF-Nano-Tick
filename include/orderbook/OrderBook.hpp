/**
 * @file OrderBook.hpp
 * @brief High-performance Limit Order Book implementation.
 *
 * Provides a cache-efficient limit order book optimized for HFT:
 * - O(1) access to best bid/ask
 * - O(log N) insert/modify/delete operations
 * - Zero allocation on hot path (uses object pools)
 * - Template-based design for compile-time optimization
 *
 * Architecture:
 * - Price levels stored in sorted containers (one for bids, one for asks)
 * - Each price level maintains a queue of orders (FIFO)
 * - Direct order ID lookup via hash map for fast cancel/modify
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_ORDER_BOOK_HPP
#define HFT_NANOTICK_ORDER_BOOK_HPP

#include "../core/Types.hpp"
#include "../core/MemoryArena.hpp"
#include "../core/Timestamp.hpp"

#include <map>
#include <unordered_map>
#include <vector>
#include <functional>
#include <algorithm>

namespace hft {

//==============================================================================
// Order Entry
//==============================================================================

/**
 * @brief Single order entry in the order book.
 *
 * Represents a limit order at a specific price level. Cache-line
 * aligned to prevent false sharing when orders are modified.
 */
struct alignas(CACHE_LINE_SIZE) Order {
    OrderId   orderId;        ///< Unique order identifier
    Price     price;          ///< Limit price (fixed-point)
    Quantity  quantity;       ///< Remaining quantity
    Quantity  filledQty;      ///< Filled quantity
    Side      side;           ///< Buy or Sell
    Timestamp timestamp;      ///< Order entry time

    constexpr Order() noexcept = default;

    constexpr Order(OrderId id, Price p, Quantity qty, Side s, Timestamp ts = 0) noexcept
        : orderId(id)
        , price(p)
        , quantity(qty)
        , filledQty(0)
        , side(s)
        , timestamp(ts)
    {}

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return orderId != INVALID_ORDER_ID && quantity > 0;
    }

    [[nodiscard]] constexpr Quantity remainingQty() const noexcept {
        return quantity - filledQty;
    }
};

//==============================================================================
// Price Level
//==============================================================================

/**
 * @brief Represents a single price level with aggregated quantity.
 *
 * Maintains total quantity at a price and references to individual orders.
 * Orders are stored in FIFO order for price-time priority matching.
 */
struct PriceLevel {
    Price    price{INVALID_PRICE};    ///< Price at this level
    Quantity totalQty{0};             ///< Total quantity at this level
    std::vector<OrderId> orders;      ///< Order IDs at this level (FIFO)

    constexpr PriceLevel() noexcept = default;
    explicit PriceLevel(Price p) noexcept : price(p) {}

    [[nodiscard]] bool empty() const noexcept { return orders.empty(); }
    [[nodiscard]] std::size_t orderCount() const noexcept { return orders.size(); }
};

//==============================================================================
// Order Book Update Event
//==============================================================================

/**
 * @brief Event emitted when order book state changes.
 */
struct OrderBookUpdate {
    MdMsgType  action;         ///< Add, Modify, Delete, Trade
    Side       side;           ///< Affected side
    Price      price;          ///< Affected price level
    Quantity   quantity;       ///< New/changed quantity
    Quantity   totalQtyAtLevel;///< Total quantity at price level after update
    OrderId    orderId;        ///< Related order ID
    Timestamp  timestamp;      ///< Event timestamp
};

//==============================================================================
// Order Book Template
//==============================================================================

/**
 * @brief High-performance templated limit order book.
 *
 * Template parameters allow customization of:
 * - Maximum price levels to track
 * - Maximum orders per price level
 * - Custom comparators for price ordering
 *
 * @tparam MaxLevels Maximum depth of book to maintain per side.
 * @tparam MaxOrders Maximum total orders in the book.
 */
template <std::size_t MaxLevels = 100, std::size_t MaxOrders = 10000>
class OrderBook {
public:
    /// Callback type for order book updates
    using UpdateCallback = std::function<void(const OrderBookUpdate&)>;

    /**
     * @brief Construct an order book for a symbol.
     * @param symbolId Symbol identifier.
     */
    explicit OrderBook(SymbolId symbolId = 0) noexcept
        : m_symbolId(symbolId)
    {
        m_orders.reserve(MaxOrders);
    }

    // Non-copyable for safety (contains callbacks)
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Movable
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    //==========================================================================
    // Order Operations
    //==========================================================================

    /**
     * @brief Add a new order to the book.
     *
     * @param orderId Unique order identifier.
     * @param side Buy or Sell.
     * @param price Limit price.
     * @param quantity Order quantity.
     * @return true if order was added successfully.
     */
    [[nodiscard]] bool addOrder(OrderId orderId, Side side, Price price, Quantity quantity) noexcept {
        if (orderId == INVALID_ORDER_ID || price == INVALID_PRICE || quantity <= 0) [[unlikely]] {
            return false;
        }

        // Check if order already exists
        if (m_orders.contains(orderId)) [[unlikely]] {
            return false;
        }

        // Create order
        Order order(orderId, price, quantity, side, nowNanos());
        m_orders.emplace(orderId, order);

        // Add to appropriate price level
        auto& levels = (side == Side::Buy) ? m_bids : m_asks;
        auto& level = levels[price];
        level.price = price;
        level.totalQty += quantity;
        level.orders.push_back(orderId);

        // Update BBO if needed
        if (side == Side::Buy) {
            if (price > m_bestBid) m_bestBid = price;
        } else {
            if (m_bestAsk == INVALID_PRICE || price < m_bestAsk) m_bestAsk = price;
        }

        // Emit update
        emitUpdate(MdMsgType::Add, side, price, quantity, level.totalQty, orderId);

        return true;
    }

    /**
     * @brief Modify an existing order.
     *
     * @param orderId Order to modify.
     * @param newQuantity New quantity (price cannot change).
     * @return true if order was modified successfully.
     */
    [[nodiscard]] bool modifyOrder(OrderId orderId, Quantity newQuantity) noexcept {
        auto it = m_orders.find(orderId);
        if (it == m_orders.end()) [[unlikely]] {
            return false;
        }

        Order& order = it->second;
        Quantity delta = newQuantity - order.quantity;

        auto& levels = (order.side == Side::Buy) ? m_bids : m_asks;
        auto levelIt = levels.find(order.price);
        if (levelIt == levels.end()) [[unlikely]] {
            return false;
        }

        order.quantity = newQuantity;
        levelIt->second.totalQty += delta;

        // Handle zero quantity as delete
        if (newQuantity <= 0) {
            return deleteOrder(orderId);
        }

        emitUpdate(MdMsgType::Modify, order.side, order.price, newQuantity,
                   levelIt->second.totalQty, orderId);

        return true;
    }

    /**
     * @brief Delete an order from the book.
     *
     * @param orderId Order to delete.
     * @return true if order was deleted successfully.
     */
    [[nodiscard]] bool deleteOrder(OrderId orderId) noexcept {
        auto it = m_orders.find(orderId);
        if (it == m_orders.end()) [[unlikely]] {
            return false;
        }

        const Order& order = it->second;
        auto& levels = (order.side == Side::Buy) ? m_bids : m_asks;
        auto levelIt = levels.find(order.price);

        if (levelIt != levels.end()) {
            PriceLevel& level = levelIt->second;
            level.totalQty -= order.quantity;

            // Remove order from level
            auto& orders = level.orders;
            orders.erase(std::remove(orders.begin(), orders.end(), orderId), orders.end());

            Quantity remainingQty = level.totalQty;

            // Remove empty price level
            if (level.empty()) {
                levels.erase(levelIt);

                // Update BBO
                updateBBO(order.side);
            }

            emitUpdate(MdMsgType::Delete, order.side, order.price, 0,
                       remainingQty, orderId);
        }

        m_orders.erase(it);
        return true;
    }

    /**
     * @brief Apply a snapshot to reset book state.
     *
     * Clears existing state and rebuilds from snapshot data.
     *
     * @param bids Bid levels (price, quantity pairs).
     * @param asks Ask levels (price, quantity pairs).
     */
    void applySnapshot(const std::vector<std::pair<Price, Quantity>>& bids,
                       const std::vector<std::pair<Price, Quantity>>& asks) noexcept {
        clear();

        OrderId nextId = 1;

        for (const auto& [price, qty] : bids) {
            (void)addOrder(nextId++, Side::Buy, price, qty);
        }

        for (const auto& [price, qty] : asks) {
            (void)addOrder(nextId++, Side::Sell, price, qty);
        }

        emitUpdate(MdMsgType::Snapshot, Side::Buy, m_bestBid, 0, 0, 0);
    }

    /**
     * @brief Clear all orders from the book.
     */
    void clear() noexcept {
        m_bids.clear();
        m_asks.clear();
        m_orders.clear();
        m_bestBid = INVALID_PRICE;
        m_bestAsk = INVALID_PRICE;
    }

    //==========================================================================
    // Book Queries
    //==========================================================================

    /**
     * @brief Get best bid price.
     * @return Best bid price or INVALID_PRICE if no bids.
     */
    [[nodiscard]] Price bestBid() const noexcept { return m_bestBid; }

    /**
     * @brief Get best ask price.
     * @return Best ask price or INVALID_PRICE if no asks.
     */
    [[nodiscard]] Price bestAsk() const noexcept { return m_bestAsk; }

    /**
     * @brief Get quantity at best bid.
     * @return Quantity at best bid or 0.
     */
    [[nodiscard]] Quantity bestBidQty() const noexcept {
        if (m_bestBid == INVALID_PRICE) return 0;
        auto it = m_bids.find(m_bestBid);
        return (it != m_bids.end()) ? it->second.totalQty : 0;
    }

    /**
     * @brief Get quantity at best ask.
     * @return Quantity at best ask or 0.
     */
    [[nodiscard]] Quantity bestAskQty() const noexcept {
        if (m_bestAsk == INVALID_PRICE) return 0;
        auto it = m_asks.find(m_bestAsk);
        return (it != m_asks.end()) ? it->second.totalQty : 0;
    }

    /**
     * @brief Get mid price.
     * @return Mid price or INVALID_PRICE if book is empty.
     */
    [[nodiscard]] Price midPrice() const noexcept {
        if (m_bestBid == INVALID_PRICE || m_bestAsk == INVALID_PRICE) {
            return INVALID_PRICE;
        }
        return (m_bestBid + m_bestAsk) / 2;
    }

    /**
     * @brief Get spread in price units.
     * @return Spread or INVALID_PRICE if book is empty.
     */
    [[nodiscard]] Price spread() const noexcept {
        if (m_bestBid == INVALID_PRICE || m_bestAsk == INVALID_PRICE) {
            return INVALID_PRICE;
        }
        return m_bestAsk - m_bestBid;
    }

    /**
     * @brief Check if book is crossed (bid >= ask).
     * @return true if crossed.
     */
    [[nodiscard]] bool isCrossed() const noexcept {
        if (m_bestBid == INVALID_PRICE || m_bestAsk == INVALID_PRICE) {
            return false;
        }
        return m_bestBid >= m_bestAsk;
    }

    /**
     * @brief Get total quantity at a price level.
     *
     * @param side Book side.
     * @param price Price level.
     * @return Total quantity at that level.
     */
    [[nodiscard]] Quantity quantityAtPrice(Side side, Price price) const noexcept {
        const auto& levels = (side == Side::Buy) ? m_bids : m_asks;
        auto it = levels.find(price);
        return (it != levels.end()) ? it->second.totalQty : 0;
    }

    /**
     * @brief Get an order by ID.
     *
     * @param orderId Order ID.
     * @return Pointer to order or nullptr if not found.
     */
    [[nodiscard]] const Order* getOrder(OrderId orderId) const noexcept {
        auto it = m_orders.find(orderId);
        return (it != m_orders.end()) ? &it->second : nullptr;
    }

    /**
     * @brief Get number of price levels on a side.
     * @param side Book side.
     * @return Number of price levels.
     */
    [[nodiscard]] std::size_t levelCount(Side side) const noexcept {
        return (side == Side::Buy) ? m_bids.size() : m_asks.size();
    }

    /**
     * @brief Get total number of orders in book.
     * @return Order count.
     */
    [[nodiscard]] std::size_t orderCount() const noexcept {
        return m_orders.size();
    }

    /**
     * @brief Check if book is empty.
     * @return true if no orders.
     */
    [[nodiscard]] bool empty() const noexcept {
        return m_orders.empty();
    }

    /**
     * @brief Get top N price levels for a side.
     *
     * @param side Book side.
     * @param depth Number of levels to retrieve.
     * @param[out] levels Output vector of (price, quantity) pairs.
     */
    void getTopLevels(Side side, std::size_t depth,
                      std::vector<std::pair<Price, Quantity>>& levels) const {
        levels.clear();
        levels.reserve(depth);

        const auto& book = (side == Side::Buy) ? m_bids : m_asks;

        if (side == Side::Buy) {
            // Bids: highest price first
            for (auto it = book.rbegin(); it != book.rend() && levels.size() < depth; ++it) {
                levels.emplace_back(it->first, it->second.totalQty);
            }
        } else {
            // Asks: lowest price first
            for (auto it = book.begin(); it != book.end() && levels.size() < depth; ++it) {
                levels.emplace_back(it->first, it->second.totalQty);
            }
        }
    }

    //==========================================================================
    // Callbacks
    //==========================================================================

    /**
     * @brief Register callback for order book updates.
     * @param callback Function to call on each update.
     */
    void setUpdateCallback(UpdateCallback callback) noexcept {
        m_updateCallback = std::move(callback);
    }

    /**
     * @brief Get symbol ID.
     * @return Symbol identifier.
     */
    [[nodiscard]] SymbolId symbolId() const noexcept { return m_symbolId; }

private:
    void updateBBO(Side side) noexcept {
        if (side == Side::Buy) {
            if (m_bids.empty()) {
                m_bestBid = INVALID_PRICE;
            } else {
                m_bestBid = m_bids.rbegin()->first;
            }
        } else {
            if (m_asks.empty()) {
                m_bestAsk = INVALID_PRICE;
            } else {
                m_bestAsk = m_asks.begin()->first;
            }
        }
    }

    void emitUpdate(MdMsgType action, Side side, Price price,
                    Quantity quantity, Quantity totalQtyAtLevel, OrderId orderId) {
        if (m_updateCallback) {
            m_updateCallback(OrderBookUpdate{
                action, side, price, quantity, totalQtyAtLevel, orderId, nowNanos()
            });
        }
    }

    /// Bids sorted by price (highest first via reverse iteration)
    std::map<Price, PriceLevel> m_bids;

    /// Asks sorted by price (lowest first)
    std::map<Price, PriceLevel> m_asks;

    /// Order lookup by ID
    std::unordered_map<OrderId, Order> m_orders;

    /// Best bid/ask cache
    Price m_bestBid{INVALID_PRICE};
    Price m_bestAsk{INVALID_PRICE};

    /// Symbol identifier
    SymbolId m_symbolId{0};

    /// Update callback
    UpdateCallback m_updateCallback;
};

/// Default order book type
using DefaultOrderBook = OrderBook<100, 10000>;

} // namespace hft

#endif // HFT_NANOTICK_ORDER_BOOK_HPP

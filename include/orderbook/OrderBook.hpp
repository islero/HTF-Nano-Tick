/**
 * @file OrderBook.hpp
 * @brief Bounded limit order book with preallocated hot-path storage.
 *
 * The book keeps all orders and price levels inside fixed-size arrays sized by
 * template parameters. Add/modify/delete do not allocate memory after
 * construction. Order lookup uses an open-addressed index and price levels are
 * kept in sorted bounded arrays.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_ORDER_BOOK_HPP
#define HFT_NANOTICK_ORDER_BOOK_HPP

#include "../core/Types.hpp"
#include "../core/Timestamp.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace hft {

namespace detail {

[[nodiscard]] constexpr std::size_t nextPowerOfTwo(std::size_t value) noexcept {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1U;
    }
    return result;
}

[[nodiscard]] constexpr std::uint64_t mixOrderId(OrderId id) noexcept {
    std::uint64_t x = id + 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}

} // namespace detail

//==============================================================================
// Order Entry
//==============================================================================

/**
 * @brief Single order entry in the order book.
 */
struct alignas(CACHE_LINE_SIZE) Order {
    OrderId orderId; ///< Unique order identifier
    Price price; ///< Limit price (fixed-point)
    Quantity quantity; ///< Remaining quantity
    Quantity filledQty; ///< Filled quantity
    Side side; ///< Buy or Sell
    Timestamp timestamp; ///< Order entry time

    constexpr Order() noexcept = default;

    constexpr Order(OrderId id, Price p, Quantity qty, Side s, Timestamp ts = 0) noexcept
        : orderId(id), price(p), quantity(qty), filledQty(0), side(s), timestamp(ts) {}

    [[nodiscard]] constexpr bool isValid() const noexcept { return orderId != INVALID_ORDER_ID && quantity > 0; }

    [[nodiscard]] constexpr Quantity remainingQty() const noexcept { return quantity - filledQty; }
};

//==============================================================================
// Price Level
//==============================================================================

/**
 * @brief Aggregated state for one price level.
 */
struct PriceLevel {
    Price price{INVALID_PRICE}; ///< Price at this level
    Quantity totalQty{0}; ///< Total quantity at this level
    std::size_t orders{0}; ///< Number of FIFO orders at this level

    constexpr PriceLevel() noexcept = default;
    explicit constexpr PriceLevel(Price p) noexcept : price(p) {}

    [[nodiscard]] constexpr bool empty() const noexcept { return orders == 0; }
    [[nodiscard]] constexpr std::size_t orderCount() const noexcept { return orders; }
};

//==============================================================================
// Order Book Update Event
//==============================================================================

/**
 * @brief Event emitted when order book state changes.
 */
struct OrderBookUpdate {
    MdMsgType action; ///< Add, Modify, Delete, Trade
    Side side; ///< Affected side
    Price price; ///< Affected price level
    Quantity quantity; ///< New/changed quantity
    Quantity totalQtyAtLevel; ///< Total quantity at price level after update
    OrderId orderId; ///< Related order ID
    Timestamp timestamp; ///< Event timestamp
};

//==============================================================================
// Order Book Template
//==============================================================================

/**
 * @brief Bounded, allocation-free limit order book.
 *
 * @tparam MaxLevels Maximum price levels to maintain per side.
 * @tparam MaxOrders Maximum total live orders in the book.
 */
template <std::size_t MaxLevels = 100, std::size_t MaxOrders = 10000> class OrderBook {
public:
    static_assert(MaxLevels > 0, "MaxLevels must be greater than zero");
    static_assert(MaxOrders > 0, "MaxOrders must be greater than zero");

    /// Callback type for order book updates. Install outside the hot path.
    using UpdateCallback = std::function<void(const OrderBookUpdate&)>;

    explicit OrderBook(SymbolId symbolId = 0) noexcept : m_symbolId(symbolId) { resetStorage(); }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    //==========================================================================
    // Order Operations
    //==========================================================================

    [[nodiscard]] bool addOrder(OrderId orderId, Side side, Price price, Quantity quantity) noexcept {
        if (orderId == INVALID_ORDER_ID || price == INVALID_PRICE || quantity <= 0) [[unlikely]] {
            return false;
        }

        if (findOrderSlot(orderId) != INVALID_INDEX) [[unlikely]] {
            return false;
        }

        const std::size_t slotIndex = allocateOrderSlot();
        if (slotIndex == INVALID_INDEX) [[unlikely]] {
            return false;
        }

        std::size_t levelIndex = findOrInsertLevel(side, price);
        if (levelIndex == INVALID_INDEX) [[unlikely]] {
            releaseOrderSlot(slotIndex);
            return false;
        }

        OrderSlot& slot = m_orderSlots[slotIndex];
        slot.order = Order(orderId, price, quantity, side, nowNanos());
        slot.levelIndex = levelIndex;
        slot.next = INVALID_INDEX;
        slot.prev = INVALID_INDEX;
        slot.active = true;

        LevelSlot& level = levelSlot(side, levelIndex);
        appendOrderToLevel(level, slotIndex);
        level.level.totalQty += quantity;
        ++level.level.orders;

        if (!insertOrderIndex(orderId, slotIndex)) [[unlikely]] {
            unlinkOrderFromLevel(level, slotIndex);
            level.level.totalQty -= quantity;
            --level.level.orders;
            removeLevelIfEmpty(side, levelIndex);
            releaseOrderSlot(slotIndex);
            return false;
        }

        ++m_orderCount;
        updateBBO(side);
        emitUpdate(MdMsgType::Add, side, price, quantity, level.level.totalQty, orderId);

        return true;
    }

    [[nodiscard]] bool modifyOrder(OrderId orderId, Quantity newQuantity) noexcept {
        if (newQuantity <= 0) {
            return deleteOrder(orderId);
        }

        const std::size_t slotIndex = findOrderSlot(orderId);
        if (slotIndex == INVALID_INDEX) [[unlikely]] {
            return false;
        }

        OrderSlot& slot = m_orderSlots[slotIndex];
        Order& order = slot.order;
        LevelSlot& level = levelSlot(order.side, slot.levelIndex);

        const Quantity delta = newQuantity - order.quantity;
        order.quantity = newQuantity;
        level.level.totalQty += delta;

        emitUpdate(MdMsgType::Modify, order.side, order.price, newQuantity, level.level.totalQty, orderId);

        return true;
    }

    [[nodiscard]] bool deleteOrder(OrderId orderId) noexcept {
        const std::size_t slotIndex = findOrderSlot(orderId);
        if (slotIndex == INVALID_INDEX) [[unlikely]] {
            return false;
        }

        OrderSlot& slot = m_orderSlots[slotIndex];
        const Side side = slot.order.side;
        const Price price = slot.order.price;
        const Quantity quantity = slot.order.quantity;
        const std::size_t levelIndex = slot.levelIndex;

        LevelSlot& level = levelSlot(side, levelIndex);
        if (level.level.totalQty >= quantity) {
            level.level.totalQty -= quantity;
        } else {
            level.level.totalQty = 0;
        }

        unlinkOrderFromLevel(level, slotIndex);
        if (level.level.orders > 0) {
            --level.level.orders;
        }
        const Quantity remainingQty = level.level.totalQty;

        eraseOrderIndex(orderId);
        releaseOrderSlot(slotIndex);
        --m_orderCount;

        removeLevelIfEmpty(side, levelIndex);
        updateBBO(side);

        emitUpdate(MdMsgType::Delete, side, price, 0, remainingQty, orderId);

        return true;
    }

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

    void clear() noexcept { resetStorage(); }

    //==========================================================================
    // Book Queries
    //==========================================================================

    [[nodiscard]] Price bestBid() const noexcept { return m_bestBid; }
    [[nodiscard]] Price bestAsk() const noexcept { return m_bestAsk; }

    [[nodiscard]] Quantity bestBidQty() const noexcept {
        return m_bidLevelCount == 0 ? 0 : m_bidLevels[0].level.totalQty;
    }

    [[nodiscard]] Quantity bestAskQty() const noexcept {
        return m_askLevelCount == 0 ? 0 : m_askLevels[0].level.totalQty;
    }

    [[nodiscard]] Price midPrice() const noexcept {
        if (m_bestBid == INVALID_PRICE || m_bestAsk == INVALID_PRICE) {
            return INVALID_PRICE;
        }
        return (m_bestBid + m_bestAsk) / 2;
    }

    [[nodiscard]] Price spread() const noexcept {
        if (m_bestBid == INVALID_PRICE || m_bestAsk == INVALID_PRICE) {
            return INVALID_PRICE;
        }
        return m_bestAsk - m_bestBid;
    }

    [[nodiscard]] bool isCrossed() const noexcept {
        if (m_bestBid == INVALID_PRICE || m_bestAsk == INVALID_PRICE) {
            return false;
        }
        return m_bestBid >= m_bestAsk;
    }

    [[nodiscard]] Quantity quantityAtPrice(Side side, Price price) const noexcept {
        const std::size_t index = findLevel(side, price);
        return index == INVALID_INDEX ? 0 : levelSlot(side, index).level.totalQty;
    }

    [[nodiscard]] const Order* getOrder(OrderId orderId) const noexcept {
        const std::size_t slotIndex = findOrderSlot(orderId);
        return slotIndex == INVALID_INDEX ? nullptr : &m_orderSlots[slotIndex].order;
    }

    [[nodiscard]] std::size_t levelCount(Side side) const noexcept {
        return side == Side::Buy ? m_bidLevelCount : m_askLevelCount;
    }

    [[nodiscard]] std::size_t orderCount() const noexcept { return m_orderCount; }

    [[nodiscard]] bool empty() const noexcept { return m_orderCount == 0; }

    void getTopLevels(Side side, std::size_t depth, std::vector<std::pair<Price, Quantity>>& levels) const {
        levels.clear();
        levels.reserve(depth);

        const auto& sideLevels = side == Side::Buy ? m_bidLevels : m_askLevels;
        const std::size_t count = side == Side::Buy ? m_bidLevelCount : m_askLevelCount;
        const std::size_t outputCount = std::min(depth, count);

        for (std::size_t i = 0; i < outputCount; ++i) {
            levels.emplace_back(sideLevels[i].level.price, sideLevels[i].level.totalQty);
        }
    }

    //==========================================================================
    // Callbacks
    //==========================================================================

    void setUpdateCallback(UpdateCallback callback) noexcept { m_updateCallback = std::move(callback); }

    [[nodiscard]] SymbolId symbolId() const noexcept { return m_symbolId; }
    [[nodiscard]] static constexpr std::size_t maxLevels() noexcept { return MaxLevels; }
    [[nodiscard]] static constexpr std::size_t maxOrders() noexcept { return MaxOrders; }

private:
    static constexpr std::size_t INVALID_INDEX = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t ORDER_INDEX_CAPACITY = detail::nextPowerOfTwo(MaxOrders * 2U);
    static constexpr std::uint8_t INDEX_EMPTY = 0;
    static constexpr std::uint8_t INDEX_OCCUPIED = 1;
    static constexpr std::uint8_t INDEX_DELETED = 2;

    struct LevelSlot {
        PriceLevel level{};
        std::size_t firstOrderSlot{INVALID_INDEX};
        std::size_t lastOrderSlot{INVALID_INDEX};
        bool active{false};
    };

    struct OrderSlot {
        Order order{};
        std::size_t next{INVALID_INDEX};
        std::size_t prev{INVALID_INDEX};
        std::size_t levelIndex{INVALID_INDEX};
        bool active{false};
    };

    struct OrderIndexEntry {
        OrderId orderId{INVALID_ORDER_ID};
        std::size_t slotIndex{INVALID_INDEX};
        std::uint8_t state{INDEX_EMPTY};
    };

    void resetStorage() noexcept {
        m_bidLevelCount = 0;
        m_askLevelCount = 0;
        m_orderCount = 0;
        m_bestBid = INVALID_PRICE;
        m_bestAsk = INVALID_PRICE;

        m_freeOrderCount = MaxOrders;
        for (std::size_t i = 0; i < MaxOrders; ++i) {
            m_freeOrderSlots[i] = MaxOrders - 1U - i;
            m_orderSlots[i] = OrderSlot{};
        }

        for (auto& level : m_bidLevels) {
            level = LevelSlot{};
        }
        for (auto& level : m_askLevels) {
            level = LevelSlot{};
        }
        for (auto& entry : m_orderIndex) {
            entry = OrderIndexEntry{};
        }
    }

    [[nodiscard]] std::size_t allocateOrderSlot() noexcept {
        if (m_freeOrderCount == 0) {
            return INVALID_INDEX;
        }
        return m_freeOrderSlots[--m_freeOrderCount];
    }

    void releaseOrderSlot(std::size_t slotIndex) noexcept {
        m_orderSlots[slotIndex] = OrderSlot{};
        m_freeOrderSlots[m_freeOrderCount++] = slotIndex;
    }

    [[nodiscard]] LevelSlot& levelSlot(Side side, std::size_t index) noexcept {
        return side == Side::Buy ? m_bidLevels[index] : m_askLevels[index];
    }

    [[nodiscard]] const LevelSlot& levelSlot(Side side, std::size_t index) const noexcept {
        return side == Side::Buy ? m_bidLevels[index] : m_askLevels[index];
    }

    [[nodiscard]] std::size_t findLevel(Side side, Price price) const noexcept {
        const auto& levels = side == Side::Buy ? m_bidLevels : m_askLevels;
        const std::size_t count = side == Side::Buy ? m_bidLevelCount : m_askLevelCount;

        for (std::size_t i = 0; i < count; ++i) {
            if (levels[i].level.price == price) {
                return i;
            }
        }
        return INVALID_INDEX;
    }

    [[nodiscard]] std::size_t findOrInsertLevel(Side side, Price price) noexcept {
        const std::size_t existing = findLevel(side, price);
        if (existing != INVALID_INDEX) {
            return existing;
        }

        auto& levels = side == Side::Buy ? m_bidLevels : m_askLevels;
        std::size_t& count = side == Side::Buy ? m_bidLevelCount : m_askLevelCount;

        if (count >= MaxLevels) {
            return INVALID_INDEX;
        }

        std::size_t insertPos = 0;
        if (side == Side::Buy) {
            while (insertPos < count && levels[insertPos].level.price > price) {
                ++insertPos;
            }
        } else {
            while (insertPos < count && levels[insertPos].level.price < price) {
                ++insertPos;
            }
        }

        for (std::size_t i = count; i > insertPos; --i) {
            levels[i] = levels[i - 1U];
            updateLevelLinks(i, levels[i]);
        }

        levels[insertPos] = LevelSlot{};
        levels[insertPos].level.price = price;
        levels[insertPos].active = true;
        ++count;

        return insertPos;
    }

    void removeLevelIfEmpty(Side side, std::size_t levelIndex) noexcept {
        auto& levels = side == Side::Buy ? m_bidLevels : m_askLevels;
        std::size_t& count = side == Side::Buy ? m_bidLevelCount : m_askLevelCount;

        if (levelIndex >= count || !levels[levelIndex].level.empty()) {
            return;
        }

        for (std::size_t i = levelIndex; i + 1U < count; ++i) {
            levels[i] = levels[i + 1U];
            updateLevelLinks(i, levels[i]);
        }

        --count;
        levels[count] = LevelSlot{};
    }

    void updateLevelLinks(std::size_t levelIndex, const LevelSlot& level) noexcept {
        std::size_t slotIndex = level.firstOrderSlot;
        while (slotIndex != INVALID_INDEX) {
            m_orderSlots[slotIndex].levelIndex = levelIndex;
            slotIndex = m_orderSlots[slotIndex].next;
        }
    }

    void appendOrderToLevel(LevelSlot& level, std::size_t slotIndex) noexcept {
        OrderSlot& slot = m_orderSlots[slotIndex];
        slot.prev = level.lastOrderSlot;
        slot.next = INVALID_INDEX;

        if (level.lastOrderSlot != INVALID_INDEX) {
            m_orderSlots[level.lastOrderSlot].next = slotIndex;
        } else {
            level.firstOrderSlot = slotIndex;
        }

        level.lastOrderSlot = slotIndex;
    }

    void unlinkOrderFromLevel(LevelSlot& level, std::size_t slotIndex) noexcept {
        OrderSlot& slot = m_orderSlots[slotIndex];

        if (slot.prev != INVALID_INDEX) {
            m_orderSlots[slot.prev].next = slot.next;
        } else {
            level.firstOrderSlot = slot.next;
        }

        if (slot.next != INVALID_INDEX) {
            m_orderSlots[slot.next].prev = slot.prev;
        } else {
            level.lastOrderSlot = slot.prev;
        }

        slot.next = INVALID_INDEX;
        slot.prev = INVALID_INDEX;
    }

    [[nodiscard]] std::size_t indexBucket(OrderId orderId) const noexcept {
        return static_cast<std::size_t>(detail::mixOrderId(orderId)) & (ORDER_INDEX_CAPACITY - 1U);
    }

    [[nodiscard]] std::size_t findOrderSlot(OrderId orderId) const noexcept {
        std::size_t index = indexBucket(orderId);

        for (std::size_t probe = 0; probe < ORDER_INDEX_CAPACITY; ++probe) {
            const OrderIndexEntry& entry = m_orderIndex[index];
            if (entry.state == INDEX_EMPTY) {
                return INVALID_INDEX;
            }
            if (entry.state == INDEX_OCCUPIED && entry.orderId == orderId) {
                return entry.slotIndex;
            }
            index = (index + 1U) & (ORDER_INDEX_CAPACITY - 1U);
        }

        return INVALID_INDEX;
    }

    [[nodiscard]] bool insertOrderIndex(OrderId orderId, std::size_t slotIndex) noexcept {
        std::size_t index = indexBucket(orderId);
        std::size_t firstDeleted = INVALID_INDEX;

        for (std::size_t probe = 0; probe < ORDER_INDEX_CAPACITY; ++probe) {
            OrderIndexEntry& entry = m_orderIndex[index];
            if (entry.state == INDEX_OCCUPIED && entry.orderId == orderId) {
                return false;
            }
            if (entry.state == INDEX_DELETED && firstDeleted == INVALID_INDEX) {
                firstDeleted = index;
            }
            if (entry.state == INDEX_EMPTY) {
                OrderIndexEntry& target = firstDeleted == INVALID_INDEX ? entry : m_orderIndex[firstDeleted];
                target.orderId = orderId;
                target.slotIndex = slotIndex;
                target.state = INDEX_OCCUPIED;
                return true;
            }
            index = (index + 1U) & (ORDER_INDEX_CAPACITY - 1U);
        }

        if (firstDeleted != INVALID_INDEX) {
            OrderIndexEntry& target = m_orderIndex[firstDeleted];
            target.orderId = orderId;
            target.slotIndex = slotIndex;
            target.state = INDEX_OCCUPIED;
            return true;
        }

        return false;
    }

    void eraseOrderIndex(OrderId orderId) noexcept {
        std::size_t index = indexBucket(orderId);

        for (std::size_t probe = 0; probe < ORDER_INDEX_CAPACITY; ++probe) {
            OrderIndexEntry& entry = m_orderIndex[index];
            if (entry.state == INDEX_EMPTY) {
                return;
            }
            if (entry.state == INDEX_OCCUPIED && entry.orderId == orderId) {
                entry = OrderIndexEntry{};
                reinsertClusterAfterErase(index);
                return;
            }
            index = (index + 1U) & (ORDER_INDEX_CAPACITY - 1U);
        }
    }

    void reinsertClusterAfterErase(std::size_t erasedIndex) noexcept {
        std::size_t index = (erasedIndex + 1U) & (ORDER_INDEX_CAPACITY - 1U);

        while (m_orderIndex[index].state == INDEX_OCCUPIED) {
            const OrderIndexEntry entry = m_orderIndex[index];
            m_orderIndex[index] = OrderIndexEntry{};
            (void)insertOrderIndex(entry.orderId, entry.slotIndex);
            index = (index + 1U) & (ORDER_INDEX_CAPACITY - 1U);
        }
    }

    void updateBBO(Side side) noexcept {
        if (side == Side::Buy) {
            m_bestBid = m_bidLevelCount == 0 ? INVALID_PRICE : m_bidLevels[0].level.price;
        } else {
            m_bestAsk = m_askLevelCount == 0 ? INVALID_PRICE : m_askLevels[0].level.price;
        }
    }

    void emitUpdate(MdMsgType action, Side side, Price price, Quantity quantity, Quantity totalQtyAtLevel,
                    OrderId orderId) {
        if (m_updateCallback) {
            m_updateCallback(OrderBookUpdate{action, side, price, quantity, totalQtyAtLevel, orderId, nowNanos()});
        }
    }

    std::array<LevelSlot, MaxLevels> m_bidLevels{};
    std::array<LevelSlot, MaxLevels> m_askLevels{};
    std::size_t m_bidLevelCount{0};
    std::size_t m_askLevelCount{0};

    std::array<OrderSlot, MaxOrders> m_orderSlots{};
    std::array<std::size_t, MaxOrders> m_freeOrderSlots{};
    std::size_t m_freeOrderCount{0};
    std::size_t m_orderCount{0};

    std::array<OrderIndexEntry, ORDER_INDEX_CAPACITY> m_orderIndex{};

    Price m_bestBid{INVALID_PRICE};
    Price m_bestAsk{INVALID_PRICE};
    SymbolId m_symbolId{0};
    UpdateCallback m_updateCallback;
};

using DefaultOrderBook = OrderBook<100, 10000>;

} // namespace hft

#endif // HFT_NANOTICK_ORDER_BOOK_HPP

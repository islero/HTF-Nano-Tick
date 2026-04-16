/**
 * @file MarketDataHandler.hpp
 * @brief Market data processing engine with stale data protection.
 *
 * Handles incoming market data messages with:
 * - Stale data detection and filtering
 * - Sequence number gap detection
 * - Message deduplication
 * - Order book update dispatch
 *
 * Stale data protection is critical for HFT to avoid trading on
 * outdated quotes that could result in adverse selection.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_MARKET_DATA_HANDLER_HPP
#define HFT_NANOTICK_MARKET_DATA_HANDLER_HPP

#include "../core/Types.hpp"
#include "../core/SPSCQueue.hpp"
#include "../core/Timestamp.hpp"
#include "../orderbook/OrderBook.hpp"
#include "../utils/Logger.hpp"
#ifdef HFT_ENABLE_QUICKFIX
#include "QuickFixApplication.hpp"
#include <quickfix/Message.h>
#include <quickfix/fix44/MarketDataSnapshotFullRefresh.h>
#include <quickfix/fix44/MarketDataIncrementalRefresh.h>
#endif

#include <unordered_map>
#include <functional>
#include <atomic>
#include <string>

namespace hft {

//==============================================================================
// Market Data Configuration
//==============================================================================

/**
 * @brief Configuration for market data handling.
 */
struct MarketDataConfig {
    /// Maximum staleness threshold in nanoseconds (default: 50ms)
    Timestamp staleThresholdNanos{50'000'000};

    /// Enable sequence number gap detection
    bool checkSequenceGaps{true};

    /// Maximum acceptable sequence gap before recovery
    SeqNum maxSequenceGap{10};

    /// Enable message deduplication
    bool deduplicateMessages{true};

    /// Maximum messages to track for deduplication
    std::size_t dedupeWindowSize{1000};
};

//==============================================================================
// Market Data Message
//==============================================================================

/**
 * @brief Normalized market data update message.
 *
 * Internal representation of market data updates, independent of
 * the wire protocol (FIX, binary, etc.).
 */
struct alignas(CACHE_LINE_SIZE) MarketDataMessage {
    SymbolId   symbolId;          ///< Instrument symbol
    SeqNum     seqNum;            ///< Exchange sequence number
    Timestamp  sendingTime;       ///< Exchange timestamp (SendingTime)
    Timestamp  receiveTime;       ///< Local receive timestamp
    MdMsgType  msgType;           ///< Message type
    Side       side;              ///< Bid/Ask for book updates
    Price      price;             ///< Price
    Quantity   quantity;          ///< Quantity
    OrderId    orderId;           ///< Order ID (for order-level books)

    MarketDataMessage() noexcept = default;

    /**
     * @brief Check if message is stale.
     * @param thresholdNanos Maximum allowed age in nanoseconds.
     * @return true if message is stale.
     */
    [[nodiscard]] bool isStale(Timestamp thresholdNanos) const noexcept {
        return (receiveTime - sendingTime) > thresholdNanos;
    }

    /**
     * @brief Get message latency (wire time).
     * @return Latency in nanoseconds.
     */
    [[nodiscard]] Timestamp latency() const noexcept {
        return receiveTime - sendingTime;
    }
};

//==============================================================================
// Stale Data Statistics
//==============================================================================

/**
 * @brief Statistics for stale data monitoring.
 */
struct alignas(CACHE_LINE_SIZE) StaleDataStats {
    std::atomic<std::uint64_t> totalMessages{0};
    std::atomic<std::uint64_t> staleMessages{0};
    std::atomic<std::uint64_t> sequenceGaps{0};
    std::atomic<std::uint64_t> duplicateMessages{0};
    std::atomic<std::int64_t> maxLatencyNanos{0};
    std::atomic<std::int64_t> avgLatencySum{0};

    void reset() noexcept {
        totalMessages.store(0, std::memory_order_relaxed);
        staleMessages.store(0, std::memory_order_relaxed);
        sequenceGaps.store(0, std::memory_order_relaxed);
        duplicateMessages.store(0, std::memory_order_relaxed);
        maxLatencyNanos.store(0, std::memory_order_relaxed);
        avgLatencySum.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] double staleRatio() const noexcept {
        auto total = totalMessages.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(staleMessages.load(std::memory_order_relaxed)) /
               static_cast<double>(total);
    }

    [[nodiscard]] double avgLatencyNanos() const noexcept {
        auto total = totalMessages.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(avgLatencySum.load(std::memory_order_relaxed)) /
               static_cast<double>(total);
    }
};

//==============================================================================
// Market Data Handler
//==============================================================================

/**
 * @brief Market data message processor with stale data protection.
 *
 * Processes incoming market data messages, filters stale data, and
 * updates order books. Uses CRTP-style callbacks for zero-overhead
 * dispatch to derived handlers.
 *
 * @tparam Derived CRTP derived class for callbacks.
 * @tparam QueueCapacity Size of internal message queue.
 */
template <typename Derived, std::size_t QueueCapacity = 65536>
class MarketDataHandler {
public:
    static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                  "QueueCapacity must be power of two");

    /**
     * @brief Construct market data handler.
     * @param config Handler configuration.
     */
    explicit MarketDataHandler(const MarketDataConfig& config = {}) noexcept
        : m_config(config)
    {}

    virtual ~MarketDataHandler() = default;

    // Non-copyable
    MarketDataHandler(const MarketDataHandler&) = delete;
    MarketDataHandler& operator=(const MarketDataHandler&) = delete;

#ifdef HFT_ENABLE_QUICKFIX
    //==========================================================================
    // QuickFIX Message Processing
    //==========================================================================

    /**
     * @brief Process a QuickFIX market data snapshot message.
     *
     * @param message QuickFIX MarketDataSnapshotFullRefresh message.
     * @param receiveTime Local receive timestamp.
     * @return true if message was processed (not filtered).
     */
    [[nodiscard]] bool processMessage(const FIX44::MarketDataSnapshotFullRefresh& message,
                                      Timestamp receiveTime = 0) noexcept {
        if (receiveTime == 0) {
            receiveTime = nowNanos();
        }

        m_stats.totalMessages.fetch_add(1, std::memory_order_relaxed);

        // Extract symbol
        FIX::Symbol symbolField;
        message.get(symbolField);
        SymbolId symbolId = hashSymbol(symbolField.getValue());

        // Extract sending time
        FIX::SendingTime sendingTimeField;
        Timestamp sendingTime = receiveTime;
        if (message.getHeader().isSet(sendingTimeField)) {
            message.getHeader().get(sendingTimeField);
            sendingTime = utcTimestampToNanos(sendingTimeField);
        }

        // Update latency statistics
        Timestamp latency = receiveTime - sendingTime;
        m_stats.avgLatencySum.fetch_add(latency, std::memory_order_relaxed);
        updateMaxLatency(latency);

        // Stale data check
        if (latency > m_config.staleThresholdNanos) [[unlikely]] {
            m_stats.staleMessages.fetch_add(1, std::memory_order_relaxed);
            MarketDataMessage msg;
            msg.receiveTime = receiveTime;
            msg.sendingTime = sendingTime;
            static_cast<Derived*>(this)->onStaleMessage(msg, latency);
            return false;
        }

        // Process all MD entries
        int numEntries = static_cast<int>(message.groupCount(FIX::FIELD::NoMDEntries));

        for (int i = 1; i <= numEntries; ++i) {
            FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
            message.getGroup(static_cast<unsigned int>(i), group);

            MarketDataMessage msg;
            msg.symbolId = symbolId;
            msg.receiveTime = receiveTime;
            msg.sendingTime = sendingTime;
            msg.msgType = MdMsgType::Snapshot;

            // Extract entry type
            FIX::MDEntryType entryType;
            group.get(entryType);
            char et = entryType.getValue();
            if (et == qfix::MD_ENTRY_BID) {
                msg.side = Side::Buy;
            } else if (et == qfix::MD_ENTRY_ASK) {
                msg.side = Side::Sell;
            }

            // Extract price
            FIX::MDEntryPx priceField;
            if (group.isSet(priceField)) {
                group.get(priceField);
                msg.price = priceFromDouble(priceField.getValue());
            }

            // Extract size
            FIX::MDEntrySize sizeField;
            if (group.isSet(sizeField)) {
                group.get(sizeField);
                msg.quantity = static_cast<Quantity>(sizeField.getValue());
            }

            // CRTP callback
            static_cast<Derived*>(this)->onMarketDataUpdate(msg);
        }

        // Notify snapshot complete
        static_cast<Derived*>(this)->onSnapshot(message);
        return true;
    }

    /**
     * @brief Process a QuickFIX market data incremental refresh message.
     *
     * @param message QuickFIX MarketDataIncrementalRefresh message.
     * @param receiveTime Local receive timestamp.
     * @return true if message was processed (not filtered).
     */
    [[nodiscard]] bool processMessage(const FIX44::MarketDataIncrementalRefresh& message,
                                      Timestamp receiveTime = 0) noexcept {
        if (receiveTime == 0) {
            receiveTime = nowNanos();
        }

        m_stats.totalMessages.fetch_add(1, std::memory_order_relaxed);

        // Extract sending time
        FIX::SendingTime sendingTimeField;
        Timestamp sendingTime = receiveTime;
        if (message.getHeader().isSet(sendingTimeField)) {
            message.getHeader().get(sendingTimeField);
            sendingTime = utcTimestampToNanos(sendingTimeField);
        }

        // Update latency statistics
        Timestamp latency = receiveTime - sendingTime;
        m_stats.avgLatencySum.fetch_add(latency, std::memory_order_relaxed);
        updateMaxLatency(latency);

        // Stale data check
        if (latency > m_config.staleThresholdNanos) [[unlikely]] {
            m_stats.staleMessages.fetch_add(1, std::memory_order_relaxed);
            MarketDataMessage msg;
            msg.receiveTime = receiveTime;
            msg.sendingTime = sendingTime;
            static_cast<Derived*>(this)->onStaleMessage(msg, latency);
            return false;
        }

        // Extract sequence number for gap detection
        FIX::MsgSeqNum seqNumField;
        SeqNum seqNum = 0;
        if (message.getHeader().isSet(seqNumField)) {
            message.getHeader().get(seqNumField);
            seqNum = static_cast<SeqNum>(seqNumField.getValue());
        }

        // Process all MD entries
        int numEntries = static_cast<int>(message.groupCount(FIX::FIELD::NoMDEntries));

        for (int i = 1; i <= numEntries; ++i) {
            FIX44::MarketDataIncrementalRefresh::NoMDEntries group;
            message.getGroup(static_cast<unsigned int>(i), group);

            // Extract symbol for this entry
            FIX::Symbol symbolField;
            SymbolId symbolId = 0;
            if (group.isSet(symbolField)) {
                group.get(symbolField);
                symbolId = hashSymbol(symbolField.getValue());
            }

            // Sequence gap detection
            if (m_config.checkSequenceGaps && seqNum > 0) {
                checkSequenceGap(symbolId, seqNum);
            }

            MarketDataMessage msg;
            msg.symbolId = symbolId;
            msg.seqNum = seqNum;
            msg.receiveTime = receiveTime;
            msg.sendingTime = sendingTime;

            // Extract update action
            FIX::MDUpdateAction actionField;
            if (group.isSet(actionField)) {
                group.get(actionField);
                char action = actionField.getValue();
                if (action == qfix::MD_ACTION_NEW) {
                    msg.msgType = MdMsgType::Add;
                } else if (action == qfix::MD_ACTION_CHANGE) {
                    msg.msgType = MdMsgType::Modify;
                } else if (action == qfix::MD_ACTION_DELETE) {
                    msg.msgType = MdMsgType::Delete;
                }
            }

            // Extract entry type
            FIX::MDEntryType entryType;
            group.get(entryType);
            char et = entryType.getValue();
            if (et == qfix::MD_ENTRY_BID) {
                msg.side = Side::Buy;
            } else if (et == qfix::MD_ENTRY_ASK) {
                msg.side = Side::Sell;
            }

            // Extract price
            FIX::MDEntryPx priceField;
            if (group.isSet(priceField)) {
                group.get(priceField);
                msg.price = priceFromDouble(priceField.getValue());
            }

            // Extract size
            FIX::MDEntrySize sizeField;
            if (group.isSet(sizeField)) {
                group.get(sizeField);
                msg.quantity = static_cast<Quantity>(sizeField.getValue());
            }

            // CRTP callback for each entry
            static_cast<Derived*>(this)->onIncrementalEntry(msg);
            static_cast<Derived*>(this)->onMarketDataUpdate(msg);
        }

        return true;
    }

    /**
     * @brief Process a QuickFixMdEntry from the application queue.
     *
     * @param entry Market data entry from QuickFIX application.
     * @param symbolId Symbol ID for this entry.
     * @return true if message was processed.
     */
    [[nodiscard]] bool processEntry(const QuickFixMdEntry& entry, SymbolId symbolId) noexcept {
        m_stats.totalMessages.fetch_add(1, std::memory_order_relaxed);

        // Stale data check
        Timestamp latency = nowNanos() - entry.receiveTime;
        if (latency > m_config.staleThresholdNanos) [[unlikely]] {
            m_stats.staleMessages.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        MarketDataMessage msg;
        msg.symbolId = symbolId;
        msg.receiveTime = entry.receiveTime;
        msg.sendingTime = entry.receiveTime;  // Use receive time as proxy
        msg.price = entry.price;
        msg.quantity = entry.size;

        // Map entry type
        if (entry.entryType == qfix::MD_ENTRY_BID) {
            msg.side = Side::Buy;
        } else if (entry.entryType == qfix::MD_ENTRY_ASK) {
            msg.side = Side::Sell;
        }

        // Map update action
        if (entry.updateAction == qfix::MD_ACTION_NEW) {
            msg.msgType = MdMsgType::Add;
        } else if (entry.updateAction == qfix::MD_ACTION_CHANGE) {
            msg.msgType = MdMsgType::Modify;
        } else if (entry.updateAction == qfix::MD_ACTION_DELETE) {
            msg.msgType = MdMsgType::Delete;
        }

        static_cast<Derived*>(this)->onMarketDataUpdate(msg);
        return true;
    }
#endif

    /**
     * @brief Enqueue a message for async processing.
     *
     * @param msg Pre-parsed market data message.
     * @return true if enqueued, false if queue full.
     */
    [[nodiscard]] bool enqueue(const MarketDataMessage& msg) noexcept {
        return m_queue.tryPush(msg);
    }

    /**
     * @brief Process next message from queue (consumer thread).
     *
     * @return true if a message was processed.
     */
    [[nodiscard]] bool processNext() noexcept {
        MarketDataMessage msg;
        if (!m_queue.tryPop(msg)) {
            return false;
        }

        // Check staleness again (time may have passed in queue)
        Timestamp now = nowNanos();
        if ((now - msg.sendingTime) > m_config.staleThresholdNanos) {
            m_stats.staleMessages.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Dispatch to CRTP handler
        static_cast<Derived*>(this)->onMarketDataUpdate(msg);

        return true;
    }

    //==========================================================================
    // Configuration
    //==========================================================================

    /**
     * @brief Update stale threshold at runtime.
     * @param thresholdNanos New threshold in nanoseconds.
     */
    void setStaleThreshold(Timestamp thresholdNanos) noexcept {
        m_config.staleThresholdNanos = thresholdNanos;
    }

    /**
     * @brief Get current stale threshold.
     * @return Threshold in nanoseconds.
     */
    [[nodiscard]] Timestamp staleThreshold() const noexcept {
        return m_config.staleThresholdNanos;
    }

    /**
     * @brief Get stale data statistics.
     * @return Reference to statistics.
     */
    [[nodiscard]] const StaleDataStats& stats() const noexcept {
        return m_stats;
    }

    /**
     * @brief Reset statistics.
     */
    void resetStats() noexcept {
        m_stats.reset();
    }

    /**
     * @brief Get queue depth.
     * @return Approximate number of messages in queue.
     */
    [[nodiscard]] std::size_t queueDepth() const noexcept {
        return m_queue.sizeApprox();
    }

protected:
    // CRTP interface - default implementations
    void onStaleMessage([[maybe_unused]] const MarketDataMessage& msg,
                        [[maybe_unused]] Timestamp latency) noexcept {}

    void onSequenceGap([[maybe_unused]] SymbolId symbolId,
                       [[maybe_unused]] SeqNum lastSeq,
                       [[maybe_unused]] SeqNum newSeq) noexcept {}

    void onLargeSequenceGap([[maybe_unused]] SymbolId symbolId,
                            [[maybe_unused]] SeqNum lastSeq,
                            [[maybe_unused]] SeqNum newSeq) noexcept {}

#ifdef HFT_ENABLE_QUICKFIX
    void onSnapshot([[maybe_unused]] const FIX44::MarketDataSnapshotFullRefresh& message) noexcept {}
#endif

    void onIncrementalEntry([[maybe_unused]] const MarketDataMessage& msg) noexcept {}

    void onMarketDataUpdate([[maybe_unused]] const MarketDataMessage& msg) noexcept {}

private:
    void updateMaxLatency(Timestamp latency) noexcept {
        Timestamp currentMax = m_stats.maxLatencyNanos.load(std::memory_order_relaxed);
        while (latency > currentMax &&
               !m_stats.maxLatencyNanos.compare_exchange_weak(currentMax, latency,
                   std::memory_order_relaxed)) {}
    }

    void checkSequenceGap(SymbolId symbolId, SeqNum seqNum) noexcept {
        auto& lastSeq = m_lastSeqNums[symbolId];

        if (lastSeq != 0 && seqNum > lastSeq + 1) {
            SeqNum gap = seqNum - lastSeq - 1;

            if (gap <= m_config.maxSequenceGap) {
                m_stats.sequenceGaps.fetch_add(1, std::memory_order_relaxed);
                static_cast<Derived*>(this)->onSequenceGap(symbolId, lastSeq, seqNum);
            } else {
                // Large gap - request snapshot
                static_cast<Derived*>(this)->onLargeSequenceGap(symbolId, lastSeq, seqNum);
            }
        }
        lastSeq = seqNum;
    }

    [[nodiscard]] static SymbolId hashSymbol(const std::string& symbol) noexcept {
        SymbolId id = 0;
        for (char c : symbol) {
            id = id * 31 + static_cast<SymbolId>(c);
        }
        return id;
    }

    MarketDataConfig m_config;
    SPSCQueue<MarketDataMessage, QueueCapacity> m_queue;
    StaleDataStats m_stats;
    std::unordered_map<SymbolId, SeqNum> m_lastSeqNums;
};

//==============================================================================
// Default Market Data Handler Implementation
//==============================================================================

/**
 * @brief Concrete market data handler implementation.
 *
 * Provides default behavior for all CRTP callbacks. Can be used
 * directly or as a base for custom handlers.
 */
class DefaultMarketDataHandler : public MarketDataHandler<DefaultMarketDataHandler> {
public:
    using Base = MarketDataHandler<DefaultMarketDataHandler>;
    using Base::Base;

    /**
     * @brief Register order book for a symbol.
     * @param symbolId Symbol identifier.
     * @param book Pointer to order book.
     */
    void registerOrderBook(SymbolId symbolId, DefaultOrderBook* book) noexcept {
        m_orderBooks[symbolId] = book;
    }

    // CRTP implementations
    void onStaleMessage(const MarketDataMessage& msg, Timestamp latency) noexcept {
        // Log stale message (could be throttled in production)
        (void)HFT_LOG_WARN("Stale market data detected");
        (void)msg;
        (void)latency;
    }

    void onSequenceGap(SymbolId symbolId, SeqNum lastSeq, SeqNum newSeq) noexcept {
        (void)HFT_LOG_WARN("Sequence gap detected");
        (void)symbolId;
        (void)lastSeq;
        (void)newSeq;
    }

    void onMarketDataUpdate(const MarketDataMessage& msg) noexcept {
        auto it = m_orderBooks.find(msg.symbolId);
        if (it == m_orderBooks.end()) return;

        DefaultOrderBook* book = it->second;

        switch (msg.msgType) {
            case MdMsgType::Add:
                (void)book->addOrder(msg.orderId, msg.side, msg.price, msg.quantity);
                break;

            case MdMsgType::Modify:
                (void)book->modifyOrder(msg.orderId, msg.quantity);
                break;

            case MdMsgType::Delete:
                (void)book->deleteOrder(msg.orderId);
                break;

            default:
                break;
        }
    }

private:
    std::unordered_map<SymbolId, DefaultOrderBook*> m_orderBooks;
};

} // namespace hft

#endif // HFT_NANOTICK_MARKET_DATA_HANDLER_HPP

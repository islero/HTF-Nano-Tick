/**
 * @file Types.hpp
 * @brief Core type definitions for the HFT trading system.
 *
 * This file contains fundamental type aliases, enumerations, and constants
 * used throughout the trading system. All types are designed for maximum
 * performance with fixed-size representations.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_TYPES_HPP
#define HFT_NANOTICK_TYPES_HPP

#include <cstdint>
#include <cstddef>
#include <limits>
#include <array>

namespace hft {

//==============================================================================
// Hardware Constants
//==============================================================================

/// Cache line size for x86-64 architectures (64 bytes)
inline constexpr std::size_t CACHE_LINE_SIZE = 64;

/// Page size for memory alignment (4KB typical)
inline constexpr std::size_t PAGE_SIZE = 4096;

//==============================================================================
// Numeric Type Aliases
//==============================================================================

/// Price representation using fixed-point arithmetic (8 decimal places)
/// Allows prices up to ~92 billion with 8 decimal precision
using Price = std::int64_t;

/// Quantity representation (supports up to ~9.2 quintillion)
using Quantity = std::int64_t;

/// Order identifier
using OrderId = std::uint64_t;

/// Sequence number for market data
using SeqNum = std::uint64_t;

/// Timestamp in nanoseconds since epoch
using Timestamp = std::int64_t;

/// Symbol identifier (fixed 8-byte representation)
using SymbolId = std::uint32_t;

//==============================================================================
// Price Conversion Constants
//==============================================================================

/// Number of decimal places for price representation
inline constexpr int PRICE_DECIMALS = 8;

/// Multiplier for converting double to fixed-point price
inline constexpr std::int64_t PRICE_MULTIPLIER = 100'000'000LL;

/// Invalid price sentinel
inline constexpr Price INVALID_PRICE = std::numeric_limits<Price>::min();

/// Invalid quantity sentinel
inline constexpr Quantity INVALID_QUANTITY = std::numeric_limits<Quantity>::min();

/// Invalid order ID sentinel
inline constexpr OrderId INVALID_ORDER_ID = 0;

/// Invalid sequence number sentinel
inline constexpr SeqNum INVALID_SEQ_NUM = std::numeric_limits<SeqNum>::max();

//==============================================================================
// Price Conversion Utilities
//==============================================================================

/**
 * @brief Convert a double price to fixed-point representation.
 * @param price The price as a floating-point number.
 * @return Fixed-point price representation.
 */
[[nodiscard]] constexpr Price priceFromDouble(double price) noexcept {
    return static_cast<Price>(price * PRICE_MULTIPLIER);
}

/**
 * @brief Convert a fixed-point price to double.
 * @param price The fixed-point price.
 * @return Price as a floating-point number.
 */
[[nodiscard]] constexpr double priceToDouble(Price price) noexcept {
    return static_cast<double>(price) / PRICE_MULTIPLIER;
}

//==============================================================================
// Enumerations
//==============================================================================

/**
 * @brief Order side enumeration.
 */
enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

/**
 * @brief Get the opposite side.
 * @param side The current side.
 * @return The opposite side.
 */
[[nodiscard]] constexpr Side oppositeSide(Side side) noexcept { return side == Side::Buy ? Side::Sell : Side::Buy; }

/**
 * @brief Order type enumeration.
 */
enum class OrderType : std::uint8_t {
    Limit = 0,
    Market = 1,
    IOC = 2, // Immediate or Cancel
    FOK = 3 // Fill or Kill
};

/**
 * @brief Order status enumeration.
 */
enum class OrderStatus : std::uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Canceled = 3,
    Rejected = 4,
    PendingNew = 5,
    PendingCancel = 6
};

/**
 * @brief Market data message type.
 */
enum class MdMsgType : std::uint8_t { Add = 0, Modify = 1, Delete = 2, Trade = 3, Snapshot = 4, Heartbeat = 5 };

/**
 * @brief Instrument type enumeration.
 */
enum class InstrumentType : std::uint8_t { Spot = 0, Future = 1, Option = 2, Perpetual = 3 };

//==============================================================================
// Symbol Representation
//==============================================================================

/**
 * @brief Fixed-size symbol representation for cache efficiency.
 *
 * Stores up to 15 characters + null terminator, fitting within 16 bytes.
 */
struct alignas(16) Symbol {
    static constexpr std::size_t MAX_LENGTH = 15;
    std::array<char, MAX_LENGTH + 1> data{};

    constexpr Symbol() noexcept = default;

    constexpr explicit Symbol(const char* str) noexcept {
        std::size_t i = 0;
        while (i < MAX_LENGTH && str[i] != '\0') {
            data[i] = str[i];
            ++i;
        }
        data[i] = '\0';
    }

    [[nodiscard]] constexpr const char* c_str() const noexcept { return data.data(); }

    [[nodiscard]] constexpr bool operator==(const Symbol& other) const noexcept {
        for (std::size_t i = 0; i <= MAX_LENGTH; ++i) {
            if (data[i] != other.data[i]) return false;
            if (data[i] == '\0') break;
        }
        return true;
    }
};

} // namespace hft

#endif // HFT_NANOTICK_TYPES_HPP

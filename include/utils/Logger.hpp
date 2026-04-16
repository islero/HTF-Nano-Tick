/**
 * @file Logger.hpp
 * @brief Lock-free asynchronous logging system for HFT applications.
 *
 * Provides ultra-low latency logging by decoupling log message creation
 * from I/O operations. The hot path only formats and enqueues messages;
 * a background thread handles the actual file/console writes.
 *
 * Design principles:
 * - Zero allocation on log path (uses pre-allocated buffers)
 * - Lock-free message queue
 * - Compile-time log level filtering
 * - Nanosecond-precision timestamps
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_LOGGER_HPP
#define HFT_NANOTICK_LOGGER_HPP

#include "../core/Types.hpp"
#include "../core/SPSCQueue.hpp"
#include "../core/Timestamp.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
#include <fstream>
#include <charconv>

namespace hft {

//==============================================================================
// Log Level Definitions
//==============================================================================

/**
 * @brief Log severity levels.
 */
enum class LogLevel : std::uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Fatal = 5, Off = 6 };

/**
 * @brief Convert log level to string.
 * @param level The log level.
 * @return String representation.
 */
[[nodiscard]] constexpr std::string_view logLevelToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "?????";
    }
}

//==============================================================================
// Log Message Structure
//==============================================================================

/**
 * @brief Fixed-size log message for queue transport.
 *
 * Designed to fit within typical cache line sizes for efficient
 * queue operations. Message is truncated if it exceeds MAX_MSG_LEN.
 */
struct alignas(CACHE_LINE_SIZE) LogMessage {
    static constexpr std::size_t MAX_MSG_LEN = 200;

    Timestamp timestamp{0}; ///< Nanoseconds since epoch
    LogLevel level{LogLevel::Info}; ///< Severity level
    std::uint8_t msgLen{0}; ///< Actual message length
    char message[MAX_MSG_LEN]{}; ///< Message buffer

    LogMessage() noexcept = default;

    /**
     * @brief Construct a log message.
     * @param lvl Log level.
     * @param msg Message string view.
     */
    LogMessage(LogLevel lvl, std::string_view msg) noexcept
        : timestamp(nowNanos()), level(lvl), msgLen(static_cast<std::uint8_t>(std::min(msg.size(), MAX_MSG_LEN - 1))) {
        std::memcpy(message, msg.data(), msgLen);
        message[msgLen] = '\0';
    }
};

//==============================================================================
// Async Logger Implementation
//==============================================================================

/**
 * @brief Lock-free asynchronous logger.
 *
 * Uses an SPSC queue to transfer log messages from the hot path to a
 * background writer thread. The hot path only does minimal formatting
 * and a single queue push operation.
 *
 * @tparam QueueCapacity Size of the log message queue (power of two).
 */
template <std::size_t QueueCapacity = 8192> class AsyncLogger {
public:
    static_assert((QueueCapacity & (QueueCapacity - 1)) == 0, "QueueCapacity must be power of two");

    /**
     * @brief Construct the async logger.
     * @param minLevel Minimum log level to record.
     * @param filename Output file (empty for stdout only).
     */
    explicit AsyncLogger(LogLevel minLevel = LogLevel::Info, std::string_view filename = "") noexcept
        : m_minLevel(minLevel), m_running(true) {
        if (!filename.empty()) {
            m_file.open(std::string(filename), std::ios::out | std::ios::app);
        }

        m_writerThread = std::thread(&AsyncLogger::writerLoop, this);
    }

    ~AsyncLogger() {
        m_running.store(false, std::memory_order_release);
        if (m_writerThread.joinable()) {
            m_writerThread.join();
        }
        // Drain remaining messages
        drainQueue();
    }

    // Non-copyable, non-movable
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    AsyncLogger& operator=(AsyncLogger&&) = delete;

    /**
     * @brief Log a message (hot path).
     *
     * This is the primary logging interface. It performs minimal work:
     * 1. Check log level
     * 2. Create message struct
     * 3. Push to queue
     *
     * @param level Log level.
     * @param msg Message to log.
     * @return true if message was queued, false if queue full.
     */
    [[nodiscard]] bool log(LogLevel level, std::string_view msg) noexcept {
        if (level < m_minLevel.load(std::memory_order_relaxed)) [[likely]] {
            return true; // Filtered out, considered success
        }

        return m_queue.tryEmplace(level, msg);
    }

    /**
     * @brief Log a pre-formatted message with variadic args.
     *
     * Uses snprintf for formatting. For maximum performance in the hot
     * path, prefer log() with pre-formatted strings.
     */
    template <typename... Args> [[nodiscard]] bool logf(LogLevel level, const char* fmt, Args... args) noexcept {
        if (level < m_minLevel.load(std::memory_order_relaxed)) [[likely]] {
            return true;
        }

        std::array<char, LogMessage::MAX_MSG_LEN> buffer;
        int len = std::snprintf(buffer.data(), buffer.size(), fmt, args...);
        if (len < 0) [[unlikely]]
            return false;

        return m_queue.tryEmplace(level,
                                  std::string_view(buffer.data(), std::min<std::size_t>(len, buffer.size() - 1)));
    }

    // Convenience methods
    [[nodiscard]] bool trace(std::string_view msg) noexcept { return log(LogLevel::Trace, msg); }
    [[nodiscard]] bool debug(std::string_view msg) noexcept { return log(LogLevel::Debug, msg); }
    [[nodiscard]] bool info(std::string_view msg) noexcept { return log(LogLevel::Info, msg); }
    [[nodiscard]] bool warn(std::string_view msg) noexcept { return log(LogLevel::Warn, msg); }
    [[nodiscard]] bool error(std::string_view msg) noexcept { return log(LogLevel::Error, msg); }
    [[nodiscard]] bool fatal(std::string_view msg) noexcept { return log(LogLevel::Fatal, msg); }

    /**
     * @brief Set minimum log level at runtime.
     * @param level New minimum level.
     */
    void setLevel(LogLevel level) noexcept { m_minLevel.store(level, std::memory_order_relaxed); }

    /**
     * @brief Get current minimum log level.
     * @return Current level.
     */
    [[nodiscard]] LogLevel level() const noexcept { return m_minLevel.load(std::memory_order_relaxed); }

    /**
     * @brief Flush all pending messages synchronously.
     *
     * Blocks until the queue is empty. Use sparingly as this
     * defeats the purpose of async logging.
     */
    void flush() noexcept {
        while (!m_queue.empty()) {
            std::this_thread::yield();
        }
        if (m_file.is_open()) {
            m_file.flush();
        }
        std::fflush(stdout);
    }

    /**
     * @brief Get approximate number of pending messages.
     * @return Queue size.
     */
    [[nodiscard]] std::size_t pendingCount() const noexcept { return m_queue.sizeApprox(); }

private:
    void writerLoop() noexcept {
        LogMessage msg;
        std::array<char, 512> lineBuffer;

        while (m_running.load(std::memory_order_acquire) || !m_queue.empty()) {
            if (m_queue.tryPop(msg)) {
                formatAndWrite(msg, lineBuffer);
            } else {
                // Brief pause to avoid busy-spinning
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    void drainQueue() noexcept {
        LogMessage msg;
        std::array<char, 512> lineBuffer;
        while (m_queue.tryPop(msg)) {
            formatAndWrite(msg, lineBuffer);
        }
    }

    void formatAndWrite(const LogMessage& msg, std::array<char, 512>& buffer) noexcept {
        // Format: [TIMESTAMP] [LEVEL] message
        // Timestamp format: HH:MM:SS.nnnnnnnnn

        // Convert timestamp to components
        std::int64_t nanos = msg.timestamp;
        std::int64_t totalSeconds = nanos / 1'000'000'000LL;
        std::int64_t nanosRemainder = nanos % 1'000'000'000LL;

        std::int64_t hours = (totalSeconds / 3600) % 24;
        std::int64_t minutes = (totalSeconds / 60) % 60;
        std::int64_t seconds = totalSeconds % 60;

        int len = std::snprintf(buffer.data(), buffer.size(), "[%02lld:%02lld:%02lld.%09lld] [%s] %s\n",
                                static_cast<long long>(hours), static_cast<long long>(minutes),
                                static_cast<long long>(seconds), static_cast<long long>(nanosRemainder),
                                logLevelToString(msg.level).data(), msg.message);

        if (len > 0) {
            std::string_view line(buffer.data(), static_cast<std::size_t>(len));

            // Write to stdout
            std::fwrite(line.data(), 1, line.size(), stdout);

            // Write to file if open
            if (m_file.is_open()) {
                m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
            }
        }
    }

    SPSCQueue<LogMessage, QueueCapacity> m_queue;
    std::atomic<LogLevel> m_minLevel;
    std::atomic<bool> m_running;
    std::thread m_writerThread;
    std::ofstream m_file;
};

//==============================================================================
// Global Logger Instance
//==============================================================================

/// Default logger type
using DefaultLogger = AsyncLogger<8192>;

/**
 * @brief Get the global logger instance.
 *
 * Lazily initialized on first call. Thread-safe initialization
 * guaranteed by C++11 static local variable semantics.
 *
 * @return Reference to the global logger.
 */
inline DefaultLogger& getLogger() noexcept {
    static DefaultLogger logger(LogLevel::Info);
    return logger;
}

// Convenience macros for global logger
#define HFT_LOG_TRACE(msg) ::hft::getLogger().trace(msg)
#define HFT_LOG_DEBUG(msg) ::hft::getLogger().debug(msg)
#define HFT_LOG_INFO(msg) ::hft::getLogger().info(msg)
#define HFT_LOG_WARN(msg) ::hft::getLogger().warn(msg)
#define HFT_LOG_ERROR(msg) ::hft::getLogger().error(msg)
#define HFT_LOG_FATAL(msg) ::hft::getLogger().fatal(msg)

} // namespace hft

#endif // HFT_NANOTICK_LOGGER_HPP

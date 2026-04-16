/**
 * @file Timestamp.hpp
 * @brief High-precision timestamp utilities for latency measurement.
 *
 * Provides nanosecond-precision timing using RDTSC (Read Time-Stamp Counter)
 * for x86-64 or std::chrono::high_resolution_clock as fallback. Essential
 * for tick-to-trade latency measurement.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_TIMESTAMP_HPP
#define HFT_NANOTICK_TIMESTAMP_HPP

#include "Types.hpp"
#include <chrono>
#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define HFT_HAS_RDTSC 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define HFT_HAS_RDTSC 0
#else
#define HFT_HAS_RDTSC 0
#endif

namespace hft {

//==============================================================================
// RDTSC Utilities
//==============================================================================

/**
 * @brief Read the CPU timestamp counter.
 *
 * Uses RDTSC on x86-64 for maximum precision (~1ns resolution on modern CPUs).
 * Falls back to chrono on other architectures.
 *
 * @return CPU cycle count or nanoseconds timestamp.
 */
[[nodiscard]] inline std::uint64_t rdtsc() noexcept {
#if HFT_HAS_RDTSC
    // Use RDTSCP for serialized read (more accurate)
    unsigned int aux;
    return __rdtscp(&aux);
#elif defined(__aarch64__)
    // ARM64: read cycle counter
    std::uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    // Fallback to chrono
    return static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
#endif
}

/**
 * @brief Read RDTSC with full pipeline serialization.
 *
 * Uses CPUID to serialize the pipeline before reading TSC.
 * More expensive but provides tighter timing guarantees.
 *
 * @return Serialized CPU cycle count.
 */
[[nodiscard]] inline std::uint64_t rdtscFenced() noexcept {
#if HFT_HAS_RDTSC
    _mm_lfence();
    std::uint64_t tsc = __rdtsc();
    _mm_lfence();
    return tsc;
#else
    return rdtsc();
#endif
}

//==============================================================================
// Clock Utilities
//==============================================================================

/**
 * @brief Get current timestamp in nanoseconds since epoch.
 * @return Nanoseconds since Unix epoch.
 */
[[nodiscard]] inline Timestamp nowNanos() noexcept {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

/**
 * @brief Get current timestamp in microseconds since epoch.
 * @return Microseconds since Unix epoch.
 */
[[nodiscard]] inline Timestamp nowMicros() noexcept {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

/**
 * @brief Get current timestamp in milliseconds since epoch.
 * @return Milliseconds since Unix epoch.
 */
[[nodiscard]] inline Timestamp nowMillis() noexcept {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

/**
 * @brief Get monotonic timestamp for elapsed time measurement.
 * @return Monotonic nanoseconds.
 */
[[nodiscard]] inline std::int64_t steadyNanos() noexcept {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

//==============================================================================
// TSC Calibration
//==============================================================================

/**
 * @brief TSC frequency calibrator for converting cycles to nanoseconds.
 *
 * Calibrates the TSC frequency at startup to allow converting cycle
 * counts to wall-clock time. Thread-safe for reads after initialization.
 */
class TscCalibrator {
public:
    /**
     * @brief Get the singleton instance.
     * @return Reference to the calibrator.
     */
    static TscCalibrator& instance() noexcept {
        static TscCalibrator calibrator;
        return calibrator;
    }

    /**
     * @brief Convert TSC cycles to nanoseconds.
     * @param cycles Number of TSC cycles.
     * @return Equivalent nanoseconds.
     */
    [[nodiscard]] std::int64_t cyclesToNanos(std::uint64_t cycles) const noexcept {
        return static_cast<std::int64_t>(cycles * m_nanosPerCycle);
    }

    /**
     * @brief Convert nanoseconds to TSC cycles.
     * @param nanos Nanoseconds.
     * @return Equivalent TSC cycles.
     */
    [[nodiscard]] std::uint64_t nanosToCycles(std::int64_t nanos) const noexcept {
        return static_cast<std::uint64_t>(nanos * m_cyclesPerNano);
    }

    /**
     * @brief Get the TSC frequency in Hz.
     * @return TSC frequency.
     */
    [[nodiscard]] double tscFrequency() const noexcept { return m_cyclesPerNano * 1e9; }

private:
    TscCalibrator() noexcept { calibrate(); }

    void calibrate() noexcept {
        constexpr std::size_t SAMPLES = 5;
        constexpr auto SLEEP_DURATION = std::chrono::milliseconds(10);

        std::array<double, SAMPLES> measurements{};

        for (std::size_t i = 0; i < SAMPLES; ++i) {
            auto start_wall = std::chrono::steady_clock::now();
            std::uint64_t start_tsc = rdtsc();

            std::this_thread::sleep_for(SLEEP_DURATION);

            std::uint64_t end_tsc = rdtsc();
            auto end_wall = std::chrono::steady_clock::now();

            auto wall_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end_wall - start_wall).count();
            std::uint64_t tsc_delta = end_tsc - start_tsc;

            if (wall_nanos > 0) {
                measurements[i] = static_cast<double>(tsc_delta) / static_cast<double>(wall_nanos);
            }
        }

        // Use median for robustness
        std::sort(measurements.begin(), measurements.end());
        m_cyclesPerNano = measurements[SAMPLES / 2];
        m_nanosPerCycle = 1.0 / m_cyclesPerNano;
    }

    double m_cyclesPerNano{3.0}; // Default ~3GHz
    double m_nanosPerCycle{0.333};
};

//==============================================================================
// Latency Histogram
//==============================================================================

/**
 * @brief Lock-free latency histogram for tick-to-trade metrics.
 *
 * Records latency measurements in predefined buckets for fast O(1)
 * recording. Supports percentile calculations for p50, p90, p99, p999.
 *
 * @tparam MaxLatencyNanos Maximum recordable latency in nanoseconds.
 * @tparam BucketCount Number of histogram buckets.
 */
template <std::int64_t MaxLatencyNanos = 10'000'000, std::size_t BucketCount = 1000>
class alignas(CACHE_LINE_SIZE) LatencyHistogram {
public:
    static_assert(MaxLatencyNanos > 0, "Max latency must be positive");
    static_assert(BucketCount > 0, "Bucket count must be positive");

    /// Nanoseconds per bucket
    static constexpr std::int64_t NANOS_PER_BUCKET = MaxLatencyNanos / BucketCount;

    LatencyHistogram() noexcept { reset(); }

    /**
     * @brief Record a latency measurement.
     * @param latencyNanos Latency in nanoseconds.
     */
    void record(std::int64_t latencyNanos) noexcept {
        if (latencyNanos < 0) [[unlikely]]
            return;

        std::size_t bucket = static_cast<std::size_t>(latencyNanos / NANOS_PER_BUCKET);
        if (bucket >= BucketCount) [[unlikely]] {
            bucket = BucketCount - 1;
        }

        m_buckets[bucket].fetch_add(1, std::memory_order_relaxed);
        m_count.fetch_add(1, std::memory_order_relaxed);
        m_sum.fetch_add(latencyNanos, std::memory_order_relaxed);

        // Update min/max (lock-free)
        std::int64_t currentMin = m_min.load(std::memory_order_relaxed);
        while (latencyNanos < currentMin &&
               !m_min.compare_exchange_weak(currentMin, latencyNanos, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {}

        std::int64_t currentMax = m_max.load(std::memory_order_relaxed);
        while (latencyNanos > currentMax &&
               !m_max.compare_exchange_weak(currentMax, latencyNanos, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {}
    }

    /**
     * @brief Record latency using TSC cycles.
     * @param startCycles Start TSC value from rdtsc().
     * @param endCycles End TSC value from rdtsc().
     */
    void recordCycles(std::uint64_t startCycles, std::uint64_t endCycles) noexcept {
        auto& calibrator = TscCalibrator::instance();
        std::int64_t nanos = calibrator.cyclesToNanos(endCycles - startCycles);
        record(nanos);
    }

    /**
     * @brief Get the specified percentile latency.
     * @param percentile Percentile (0.0 to 1.0).
     * @return Latency at the specified percentile in nanoseconds.
     */
    [[nodiscard]] std::int64_t percentile(double percentile) const noexcept {
        std::uint64_t count = m_count.load(std::memory_order_relaxed);
        if (count == 0) return 0;

        std::uint64_t target = static_cast<std::uint64_t>(count * percentile);
        std::uint64_t cumulative = 0;

        for (std::size_t i = 0; i < BucketCount; ++i) {
            cumulative += m_buckets[i].load(std::memory_order_relaxed);
            if (cumulative >= target) {
                const auto bucketUpperBound = static_cast<std::int64_t>((i + 1) * NANOS_PER_BUCKET);
                return std::min(bucketUpperBound, max());
            }
        }

        return MaxLatencyNanos;
    }

    /// Get p50 latency
    [[nodiscard]] std::int64_t p50() const noexcept { return percentile(0.50); }

    /// Get p90 latency
    [[nodiscard]] std::int64_t p90() const noexcept { return percentile(0.90); }

    /// Get p99 latency
    [[nodiscard]] std::int64_t p99() const noexcept { return percentile(0.99); }

    /// Get p99.9 latency
    [[nodiscard]] std::int64_t p999() const noexcept { return percentile(0.999); }

    /// Get minimum latency
    [[nodiscard]] std::int64_t min() const noexcept { return m_min.load(std::memory_order_relaxed); }

    /// Get maximum latency
    [[nodiscard]] std::int64_t max() const noexcept { return m_max.load(std::memory_order_relaxed); }

    /// Get average latency
    [[nodiscard]] double avg() const noexcept {
        std::uint64_t count = m_count.load(std::memory_order_relaxed);
        if (count == 0) return 0.0;
        return static_cast<double>(m_sum.load(std::memory_order_relaxed)) / static_cast<double>(count);
    }

    /// Get total sample count
    [[nodiscard]] std::uint64_t count() const noexcept { return m_count.load(std::memory_order_relaxed); }

    /// Reset all statistics
    void reset() noexcept {
        for (auto& bucket : m_buckets) {
            bucket.store(0, std::memory_order_relaxed);
        }
        m_count.store(0, std::memory_order_relaxed);
        m_sum.store(0, std::memory_order_relaxed);
        m_min.store(std::numeric_limits<std::int64_t>::max(), std::memory_order_relaxed);
        m_max.store(0, std::memory_order_relaxed);
    }

private:
    alignas(CACHE_LINE_SIZE) std::array<std::atomic<std::uint64_t>, BucketCount> m_buckets;
    alignas(CACHE_LINE_SIZE) std::atomic<std::uint64_t> m_count{0};
    alignas(CACHE_LINE_SIZE) std::atomic<std::int64_t> m_sum{0};
    alignas(CACHE_LINE_SIZE) std::atomic<std::int64_t> m_min{std::numeric_limits<std::int64_t>::max()};
    alignas(CACHE_LINE_SIZE) std::atomic<std::int64_t> m_max{0};
};

/// Default latency histogram type (max 10ms, 1000 buckets = 10us resolution)
using DefaultLatencyHistogram = LatencyHistogram<10'000'000, 1000>;

} // namespace hft

#endif // HFT_NANOTICK_TIMESTAMP_HPP

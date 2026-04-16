/**
 * @file SPSCQueue.hpp
 * @brief Lock-free Single Producer Single Consumer (SPSC) ring buffer.
 *
 * This queue provides guaranteed wait-free progress for both producer and
 * consumer threads. It uses cache-line padding to prevent false sharing
 * and power-of-two sizing for efficient modulo operations.
 *
 * Performance characteristics:
 * - O(1) push and pop operations
 * - Zero dynamic memory allocation after construction
 * - Wait-free for both producer and consumer
 * - Cache-line aligned to prevent false sharing
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_SPSC_QUEUE_HPP
#define HFT_NANOTICK_SPSC_QUEUE_HPP

#include "Types.hpp"
#include <atomic>
#include <array>
#include <optional>
#include <new>
#include <type_traits>
#include <bit>
#include <memory>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace hft {

//==============================================================================
// SPSC Queue Implementation
//==============================================================================

/**
 * @brief Lock-free Single Producer Single Consumer queue.
 *
 * Implementation uses Lamport's single-producer single-consumer queue
 * algorithm with acquire-release memory ordering for optimal performance.
 *
 * Design decisions:
 * - Power-of-two capacity for fast modulo via bitwise AND
 * - Separate cache lines for head/tail to prevent false sharing
 * - Slots aligned to cache line boundaries
 * - Move semantics supported for efficient object transfer
 *
 * @tparam T The element type (must be movable or copyable).
 * @tparam Capacity Queue capacity (must be power of two).
 */
template <typename T, std::size_t Capacity> class alignas(CACHE_LINE_SIZE) SPSCQueue {
public:
    static_assert(Capacity > 0, "Capacity must be greater than 0");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");
    static_assert(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_copy_constructible_v<T>,
                  "T must be nothrow move or copy constructible");

    /// Mask for fast modulo (Capacity - 1)
    static constexpr std::size_t INDEX_MASK = Capacity - 1;

    /**
     * @brief Construct an empty SPSC queue.
     */
    SPSCQueue() noexcept {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

    ~SPSCQueue() {
        // Destruct any remaining elements
        while (pop()) {}
    }

    // Non-copyable but movable
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    /**
     * @brief Try to push an element to the queue (producer only).
     *
     * @tparam U Forwarding reference type.
     * @param value The value to push.
     * @return true if successfully pushed, false if queue is full.
     */
    template <typename U> [[nodiscard]] bool tryPush(U&& value) noexcept {
        static_assert(std::is_convertible_v<U, T>, "U must be convertible to T");

        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t nextTail = (tail + 1) & INDEX_MASK;

        // Check if queue is full (would overwrite unread data)
        if (nextTail == m_head.load(std::memory_order_acquire)) [[unlikely]] {
            return false;
        }

        // Construct element in place
        new (&m_slots[tail].data) T(std::forward<U>(value));

        // Publish the new tail - release ensures the data write is visible
        m_tail.store(nextTail, std::memory_order_release);

        return true;
    }

    /**
     * @brief Try to push an element, constructing it in place.
     *
     * @tparam Args Constructor argument types.
     * @param args Arguments forwarded to T's constructor.
     * @return true if successfully emplaced, false if queue is full.
     */
    template <typename... Args>
    [[nodiscard]] bool tryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t nextTail = (tail + 1) & INDEX_MASK;

        if (nextTail == m_head.load(std::memory_order_acquire)) [[unlikely]] {
            return false;
        }

        new (&m_slots[tail].data) T(std::forward<Args>(args)...);
        m_tail.store(nextTail, std::memory_order_release);

        return true;
    }

    /**
     * @brief Try to pop an element from the queue (consumer only).
     *
     * @param out Reference to store the popped element.
     * @return true if successfully popped, false if queue is empty.
     */
    [[nodiscard]] bool tryPop(T& out) noexcept {
        const std::size_t head = m_head.load(std::memory_order_relaxed);

        // Check if queue is empty
        if (head == m_tail.load(std::memory_order_acquire)) [[unlikely]] {
            return false;
        }

        // Move out the data
        T* ptr = reinterpret_cast<T*>(&m_slots[head].data);
        out = std::move(*ptr);

        // Destruct the slot
        if constexpr (!std::is_trivially_destructible_v<T>) {
            ptr->~T();
        }

        // Advance head - release ensures destructor completes before slot reuse
        const std::size_t nextHead = (head + 1) & INDEX_MASK;
        m_head.store(nextHead, std::memory_order_release);

        return true;
    }

    /**
     * @brief Try to pop an element, returning it as an optional.
     *
     * @return The popped element, or std::nullopt if queue is empty.
     */
    [[nodiscard]] std::optional<T> pop() noexcept {
        T value;
        if (tryPop(value)) {
            return value;
        }
        return std::nullopt;
    }

    /**
     * @brief Peek at the front element without removing it.
     *
     * @return Pointer to front element, or nullptr if queue is empty.
     * @warning Only safe to call from consumer thread.
     */
    [[nodiscard]] const T* front() const noexcept {
        const std::size_t head = m_head.load(std::memory_order_relaxed);

        if (head == m_tail.load(std::memory_order_acquire)) {
            return nullptr;
        }

        return reinterpret_cast<const T*>(&m_slots[head].data);
    }

    /**
     * @brief Check if the queue is empty.
     *
     * @return true if empty, false otherwise.
     * @note This is a snapshot; the state may change immediately after.
     */
    [[nodiscard]] bool empty() const noexcept {
        return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
    }

    /**
     * @brief Get the approximate number of elements in the queue.
     *
     * @return Approximate element count.
     * @note This is a snapshot and may be immediately stale.
     */
    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);

        if (tail >= head) {
            return tail - head;
        }
        return Capacity - head + tail;
    }

    /**
     * @brief Get the queue capacity.
     *
     * @return Maximum number of elements (Capacity - 1 usable).
     */
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity - 1; // One slot always empty to distinguish full/empty
    }

    /**
     * @brief Check if queue is full.
     *
     * @return true if full, false otherwise.
     */
    [[nodiscard]] bool full() const noexcept {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t nextTail = (tail + 1) & INDEX_MASK;
        return nextTail == m_head.load(std::memory_order_acquire);
    }

private:
    /// Storage slot aligned to prevent false sharing between adjacent elements
    struct alignas(CACHE_LINE_SIZE) Slot {
        alignas(T) std::byte data[sizeof(T)];
    };

    // Producer-owned, updated by producer
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> m_tail{0};

    // Consumer-owned, updated by consumer
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> m_head{0};

    // Data storage - separate cache line from head/tail
    alignas(CACHE_LINE_SIZE) std::array<Slot, Capacity> m_slots;
};

//==============================================================================
// Bounded SPSC Queue with Blocking
//==============================================================================

/**
 * @brief SPSC Queue wrapper with optional spin-wait on full/empty.
 *
 * Provides push/pop methods that spin-wait when the queue is full/empty.
 * Useful when blocking is acceptable and simpler than polling.
 *
 * @tparam T Element type.
 * @tparam Capacity Queue capacity (power of two).
 * @tparam MaxSpins Maximum spin iterations before yielding.
 */
template <typename T, std::size_t Capacity, std::size_t MaxSpins = 1000>
class SPSCQueueBlocking : public SPSCQueue<T, Capacity> {
    using Base = SPSCQueue<T, Capacity>;

public:
    /**
     * @brief Push with spin-wait on full queue.
     *
     * @tparam U Forwarding reference type.
     * @param value Value to push.
     */
    template <typename U> void push(U&& value) noexcept {
        std::size_t spins = 0;
        while (!Base::tryPush(std::forward<U>(value))) {
            spinWait(spins);
        }
    }

    /**
     * @brief Emplace with spin-wait on full queue.
     *
     * @tparam Args Constructor argument types.
     * @param args Arguments forwarded to constructor.
     */
    template <typename... Args> void emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        std::size_t spins = 0;
        while (!Base::tryEmplace(std::forward<Args>(args)...)) {
            spinWait(spins);
        }
    }

    /**
     * @brief Pop with spin-wait on empty queue.
     *
     * @return The popped element.
     */
    [[nodiscard]] T popWait() noexcept {
        T value;
        std::size_t spins = 0;
        while (!Base::tryPop(value)) {
            spinWait(spins);
        }
        return value;
    }

private:
    static void spinWait(std::size_t& spins) noexcept {
        if (++spins < MaxSpins) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause(); // CPU hint for spin-wait
#elif defined(__aarch64__)
            asm volatile("yield");
#endif
        } else {
            spins = 0;
            std::this_thread::yield();
        }
    }
};

} // namespace hft

#endif // HFT_NANOTICK_SPSC_QUEUE_HPP

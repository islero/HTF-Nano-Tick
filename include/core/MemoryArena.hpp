/**
 * @file MemoryArena.hpp
 * @brief Lock-free memory arena and object pool for zero-allocation hot path.
 *
 * This file provides memory management primitives designed for HFT systems
 * where dynamic allocation on the critical path is unacceptable. The arena
 * pre-allocates memory at startup and provides O(1) allocation/deallocation.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#ifndef HFT_NANOTICK_MEMORY_ARENA_HPP
#define HFT_NANOTICK_MEMORY_ARENA_HPP

#include "Types.hpp"
#include <atomic>
#include <array>
#include <memory>
#include <new>
#include <type_traits>
#include <cassert>

namespace hft {

//==============================================================================
// Cache-Aligned Allocator
//==============================================================================

/**
 * @brief Allocator that ensures cache-line alignment.
 * @tparam T The type to allocate.
 * @tparam Alignment The alignment boundary (default: cache line size).
 */
template <typename T, std::size_t Alignment = CACHE_LINE_SIZE> class AlignedAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U> struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };

    constexpr AlignedAllocator() noexcept = default;

    template <typename U> constexpr AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    [[nodiscard]] pointer allocate(size_type n) {
        void* ptr = nullptr;
#if defined(_WIN32)
        ptr = _aligned_malloc(n * sizeof(T), Alignment);
        if (!ptr) throw std::bad_alloc();
#else
        if (posix_memalign(&ptr, Alignment, n * sizeof(T)) != 0) {
            throw std::bad_alloc();
        }
#endif
        return static_cast<pointer>(ptr);
    }

    void deallocate(pointer p, [[maybe_unused]] size_type n) noexcept {
#if defined(_WIN32)
        _aligned_free(p);
#else
        std::free(p);
#endif
    }

    template <typename U> [[nodiscard]] bool operator==(const AlignedAllocator<U, Alignment>&) const noexcept {
        return true;
    }

    template <typename U> [[nodiscard]] bool operator!=(const AlignedAllocator<U, Alignment>&) const noexcept {
        return false;
    }
};

//==============================================================================
// Fixed-Size Object Pool
//==============================================================================

/**
 * @brief Lock-free object pool for fast allocation of fixed-size objects.
 *
 * Uses a free-list with atomic operations for thread-safe allocation.
 * Objects are pre-allocated at construction time - no allocations occur
 * during normal operation.
 *
 * @tparam T The object type to pool.
 * @tparam Capacity Maximum number of objects in the pool.
 */
template <typename T, std::size_t Capacity> class alignas(CACHE_LINE_SIZE) ObjectPool {
public:
    static_assert(Capacity > 0, "Pool capacity must be greater than 0");
    static_assert(std::is_trivially_destructible_v<T> || std::is_nothrow_destructible_v<T>,
                  "T must be trivially or nothrow destructible");

    /**
     * @brief Construct the object pool, pre-allocating all slots.
     */
    ObjectPool() noexcept {
        // Initialize free list - each slot points to the next
        for (std::size_t i = 0; i < Capacity - 1; ++i) {
            m_slots[i].next = &m_slots[i + 1];
        }
        m_slots[Capacity - 1].next = nullptr;
        m_freeList.store(&m_slots[0], std::memory_order_relaxed);
        m_allocatedCount.store(0, std::memory_order_relaxed);
    }

    ~ObjectPool() = default;

    // Non-copyable, non-movable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /**
     * @brief Allocate an object from the pool.
     * @tparam Args Constructor argument types.
     * @param args Constructor arguments.
     * @return Pointer to the constructed object, or nullptr if pool exhausted.
     */
    template <typename... Args>
    [[nodiscard]] T* allocate(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        Slot* slot = popFreeSlot();
        if (!slot) [[unlikely]] {
            return nullptr;
        }

        m_allocatedCount.fetch_add(1, std::memory_order_relaxed);
        return new (&slot->storage) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Return an object to the pool.
     * @param obj Pointer to the object to deallocate.
     */
    void deallocate(T* obj) noexcept {
        if (!obj) [[unlikely]]
            return;

        if constexpr (!std::is_trivially_destructible_v<T>) {
            obj->~T();
        }

        Slot* slot = reinterpret_cast<Slot*>(obj);
        pushFreeSlot(slot);
        m_allocatedCount.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * @brief Get the number of currently allocated objects.
     * @return Number of allocated objects.
     */
    [[nodiscard]] std::size_t allocatedCount() const noexcept {
        return m_allocatedCount.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get the number of available slots.
     * @return Number of free slots.
     */
    [[nodiscard]] std::size_t availableCount() const noexcept { return Capacity - allocatedCount(); }

    /**
     * @brief Get the total pool capacity.
     * @return Pool capacity.
     */
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    union Slot {
        alignas(T) std::byte storage[sizeof(T)];
        Slot* next;
    };

    [[nodiscard]] Slot* popFreeSlot() noexcept {
        Slot* oldHead = m_freeList.load(std::memory_order_acquire);
        while (oldHead) {
            Slot* newHead = oldHead->next;
            if (m_freeList.compare_exchange_weak(oldHead, newHead, std::memory_order_release,
                                                 std::memory_order_acquire)) {
                return oldHead;
            }
        }
        return nullptr;
    }

    void pushFreeSlot(Slot* slot) noexcept {
        Slot* oldHead = m_freeList.load(std::memory_order_acquire);
        do {
            slot->next = oldHead;
        } while (
            !m_freeList.compare_exchange_weak(oldHead, slot, std::memory_order_release, std::memory_order_acquire));
    }

    alignas(CACHE_LINE_SIZE) std::atomic<Slot*> m_freeList{nullptr};
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> m_allocatedCount{0};
    alignas(CACHE_LINE_SIZE) std::array<Slot, Capacity> m_slots;
};

//==============================================================================
// Linear Arena Allocator
//==============================================================================

/**
 * @brief Fast linear allocator for temporary allocations.
 *
 * Provides O(1) allocation by simply bumping a pointer. Memory can only
 * be freed all at once by calling reset(). Ideal for per-message or
 * per-tick scratch allocations.
 *
 * @tparam Size The arena size in bytes.
 */
template <std::size_t Size> class alignas(CACHE_LINE_SIZE) LinearArena {
public:
    static_assert(Size > 0, "Arena size must be greater than 0");
    static_assert((Size & (Size - 1)) == 0 || Size % CACHE_LINE_SIZE == 0,
                  "Arena size should be a power of 2 or multiple of cache line");

    LinearArena() noexcept = default;

    // Non-copyable, non-movable
    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;
    LinearArena(LinearArena&&) = delete;
    LinearArena& operator=(LinearArena&&) = delete;

    /**
     * @brief Allocate memory from the arena.
     * @tparam Alignment Required alignment for the allocation.
     * @param size Number of bytes to allocate.
     * @return Pointer to allocated memory, or nullptr if insufficient space.
     */
    template <std::size_t Alignment = alignof(std::max_align_t)>
    [[nodiscard]] void* allocate(std::size_t size) noexcept {
        static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of 2");

        // Align the current offset
        std::size_t aligned = (m_offset + Alignment - 1) & ~(Alignment - 1);

        if (aligned + size > Size) [[unlikely]] {
            return nullptr;
        }

        void* ptr = &m_buffer[aligned];
        m_offset = aligned + size;
        return ptr;
    }

    /**
     * @brief Allocate and construct an object.
     * @tparam T The type to construct.
     * @tparam Args Constructor argument types.
     * @param args Constructor arguments.
     * @return Pointer to the constructed object.
     */
    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        void* ptr = allocate<alignof(T)>(sizeof(T));
        if (!ptr) [[unlikely]]
            return nullptr;
        return new (ptr) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Reset the arena, making all memory available again.
     *
     * Note: Does not call destructors. Caller is responsible for
     * destroying objects if needed before reset.
     */
    void reset() noexcept { m_offset = 0; }

    /**
     * @brief Get the number of bytes used.
     * @return Bytes allocated from the arena.
     */
    [[nodiscard]] std::size_t used() const noexcept { return m_offset; }

    /**
     * @brief Get the number of bytes remaining.
     * @return Bytes available for allocation.
     */
    [[nodiscard]] std::size_t remaining() const noexcept { return Size - m_offset; }

    /**
     * @brief Get the total arena capacity.
     * @return Arena size in bytes.
     */
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Size; }

private:
    alignas(CACHE_LINE_SIZE) std::array<std::byte, Size> m_buffer;
    std::size_t m_offset{0};
};

} // namespace hft

#endif // HFT_NANOTICK_MEMORY_ARENA_HPP

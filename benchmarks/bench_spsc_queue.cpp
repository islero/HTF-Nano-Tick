/**
 * @file bench_spsc_queue.cpp
 * @brief Performance benchmarks for the SPSC Queue.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#include <benchmark/benchmark.h>
#include <core/SPSCQueue.hpp>

#include <thread>
#include <atomic>

using namespace hft;

//==============================================================================
// SPSC Queue Benchmarks
//==============================================================================

static void BM_SPSCQueue_PushPop_SingleThread(benchmark::State& state) {
    SPSCQueue<int, 65536> queue;

    for (auto _ : state) {
        (void)queue.tryPush(42);
        int value;
        (void)queue.tryPop(value);
        benchmark::DoNotOptimize(value);
    }

    state.SetItemsProcessed(state.iterations() * 2);  // 2 ops per iteration
}
BENCHMARK(BM_SPSCQueue_PushPop_SingleThread);

static void BM_SPSCQueue_Push_SingleThread(benchmark::State& state) {
    SPSCQueue<int, 65536> queue;
    int value;

    for (auto _ : state) {
        (void)queue.tryPush(42);

        // Drain periodically to prevent full queue
        if (state.iterations() % 1000 == 0) {
            while (queue.tryPop(value)) {}
        }
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_Push_SingleThread);

static void BM_SPSCQueue_Emplace_SingleThread(benchmark::State& state) {
    SPSCQueue<int, 65536> queue;
    int value;

    for (auto _ : state) {
        (void)queue.tryEmplace(42);

        if (state.iterations() % 1000 == 0) {
            while (queue.tryPop(value)) {}
        }
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_Emplace_SingleThread);

static void BM_SPSCQueue_TwoThread_Throughput(benchmark::State& state) {
    SPSCQueue<int, 65536> queue;
    std::atomic<bool> running{true};
    std::atomic<int64_t> produced{0};
    std::atomic<int64_t> consumed{0};

    // Consumer thread
    std::thread consumer([&]() {
        int value;
        while (running.load(std::memory_order_acquire) || !queue.empty()) {
            if (queue.tryPop(value)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Producer (benchmark thread)
    for (auto _ : state) {
        while (!queue.tryPush(42)) {
            // Spin until space available
        }
        produced.fetch_add(1, std::memory_order_relaxed);
    }

    running.store(false, std::memory_order_release);
    consumer.join();

    state.SetItemsProcessed(state.iterations());
    state.counters["produced"] = static_cast<double>(produced.load());
    state.counters["consumed"] = static_cast<double>(consumed.load());
}
BENCHMARK(BM_SPSCQueue_TwoThread_Throughput)->UseRealTime();

static void BM_SPSCQueue_Latency(benchmark::State& state) {
    SPSCQueue<int64_t, 1024> queue;
    std::atomic<bool> running{true};

    // Consumer thread - immediately pops
    std::thread consumer([&]() {
        int64_t value;
        while (running.load(std::memory_order_acquire) || !queue.empty()) {
            (void)queue.tryPop(value);
        }
    });

    int64_t totalLatency = 0;
    int64_t samples = 0;

    for (auto _ : state) {
        auto start = std::chrono::high_resolution_clock::now();

        while (!queue.tryPush(42)) {}

        auto end = std::chrono::high_resolution_clock::now();
        totalLatency += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        ++samples;
    }

    running.store(false, std::memory_order_release);
    consumer.join();

    if (samples > 0) {
        state.counters["avg_latency_ns"] = static_cast<double>(totalLatency) / static_cast<double>(samples);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_Latency)->UseRealTime();

// Benchmark with different element sizes
template <std::size_t ElementSize>
static void BM_SPSCQueue_ElementSize(benchmark::State& state) {
    struct Element {
        std::array<char, ElementSize> data;
    };

    SPSCQueue<Element, 4096> queue;
    Element elem{};

    for (auto _ : state) {
        (void)queue.tryPush(elem);
        Element out;
        (void)queue.tryPop(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 2 * static_cast<int64_t>(ElementSize));
}
BENCHMARK_TEMPLATE(BM_SPSCQueue_ElementSize, 8);
BENCHMARK_TEMPLATE(BM_SPSCQueue_ElementSize, 64);   // Cache line
BENCHMARK_TEMPLATE(BM_SPSCQueue_ElementSize, 128);
BENCHMARK_TEMPLATE(BM_SPSCQueue_ElementSize, 256);

// Benchmark different queue sizes
static void BM_SPSCQueue_QueueSize_1K(benchmark::State& state) {
    SPSCQueue<int, 1024> queue;

    for (auto _ : state) {
        (void)queue.tryPush(42);
        int v;
        (void)queue.tryPop(v);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_SPSCQueue_QueueSize_1K);

static void BM_SPSCQueue_QueueSize_64K(benchmark::State& state) {
    SPSCQueue<int, 65536> queue;

    for (auto _ : state) {
        (void)queue.tryPush(42);
        int v;
        (void)queue.tryPop(v);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_SPSCQueue_QueueSize_64K);

// Burst benchmark - push many, then pop many
static void BM_SPSCQueue_Burst(benchmark::State& state) {
    auto burstSize = state.range(0);
    SPSCQueue<int, 65536> queue;

    for (auto _ : state) {
        // Push burst
        for (int64_t i = 0; i < burstSize; ++i) {
            (void)queue.tryPush(static_cast<int>(i));
        }

        // Pop burst
        int value;
        for (int64_t i = 0; i < burstSize; ++i) {
            (void)queue.tryPop(value);
        }
    }

    state.SetItemsProcessed(state.iterations() * burstSize * 2);
}
BENCHMARK(BM_SPSCQueue_Burst)->Range(64, 4096);

// Front peek benchmark
static void BM_SPSCQueue_FrontPeek(benchmark::State& state) {
    SPSCQueue<int, 1024> queue;
    (void)queue.tryPush(42);

    for (auto _ : state) {
        const int* front = queue.front();
        benchmark::DoNotOptimize(front);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_FrontPeek);

// Empty check benchmark
static void BM_SPSCQueue_EmptyCheck(benchmark::State& state) {
    SPSCQueue<int, 1024> queue;
    (void)queue.tryPush(42);  // Not empty

    for (auto _ : state) {
        bool empty = queue.empty();
        benchmark::DoNotOptimize(empty);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_EmptyCheck);

// Size approximation benchmark
static void BM_SPSCQueue_SizeApprox(benchmark::State& state) {
    SPSCQueue<int, 1024> queue;

    // Add some elements
    for (int i = 0; i < 100; ++i) {
        (void)queue.tryPush(i);
    }

    for (auto _ : state) {
        std::size_t size = queue.sizeApprox();
        benchmark::DoNotOptimize(size);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_SizeApprox);

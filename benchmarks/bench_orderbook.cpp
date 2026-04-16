/**
 * @file bench_orderbook.cpp
 * @brief Performance benchmarks for the OrderBook.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#include <benchmark/benchmark.h>
#include <orderbook/OrderBook.hpp>

#include <random>

using namespace hft;

//==============================================================================
// Order Book Benchmarks
//==============================================================================

static void BM_OrderBook_AddOrder(benchmark::State& state) {
    DefaultOrderBook book(1);
    OrderId orderId = 1;
    Price basePrice = priceFromDouble(50000.0);

    for (auto _ : state) {
        (void)book.addOrder(orderId++, Side::Buy, basePrice, 100);

        if (orderId % 1000 == 0) {
            book.clear();
            orderId = 1;
        }
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBook_AddOrder);

static void BM_OrderBook_AddAndDelete(benchmark::State& state) {
    DefaultOrderBook book(1);
    OrderId orderId = 1;
    Price basePrice = priceFromDouble(50000.0);

    for (auto _ : state) {
        (void)book.addOrder(orderId, Side::Buy, basePrice, 100);
        (void)book.deleteOrder(orderId);
        ++orderId;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBook_AddAndDelete);

static void BM_OrderBook_ModifyOrder(benchmark::State& state) {
    DefaultOrderBook book(1);

    // Pre-populate
    for (OrderId i = 1; i <= 1000; ++i) {
        (void)book.addOrder(i, Side::Buy, priceFromDouble(50000.0 - static_cast<double>(i)), 100);
    }

    OrderId orderId = 1;
    Quantity qty = 100;

    for (auto _ : state) {
        (void)book.modifyOrder(orderId, qty);

        ++orderId;
        if (orderId > 1000) orderId = 1;
        qty = (qty % 200) + 50;
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBook_ModifyOrder);

static void BM_OrderBook_BestBidQuery(benchmark::State& state) {
    DefaultOrderBook book(1);

    // Pre-populate with multiple levels
    for (OrderId i = 1; i <= 100; ++i) {
        (void)book.addOrder(i, Side::Buy, priceFromDouble(50000.0 - static_cast<double>(i)), 100);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(book.bestBid());
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBook_BestBidQuery);

static void BM_OrderBook_MidPriceQuery(benchmark::State& state) {
    DefaultOrderBook book(1);

    // Pre-populate both sides
    for (OrderId i = 1; i <= 50; ++i) {
        (void)book.addOrder(i, Side::Buy, priceFromDouble(50000.0 - static_cast<double>(i)), 100);
        (void)book.addOrder(i + 100, Side::Sell, priceFromDouble(50000.0 + static_cast<double>(i)), 100);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(book.midPrice());
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBook_MidPriceQuery);

static void BM_OrderBook_SpreadQuery(benchmark::State& state) {
    DefaultOrderBook book(1);

    // Pre-populate both sides
    (void)book.addOrder(1, Side::Buy, priceFromDouble(49999.0), 100);
    (void)book.addOrder(2, Side::Sell, priceFromDouble(50001.0), 100);

    for (auto _ : state) {
        benchmark::DoNotOptimize(book.spread());
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBook_SpreadQuery);

static void BM_OrderBook_DeepBook_Add(benchmark::State& state) {
    auto numLevels = state.range(0);
    DefaultOrderBook book(1);
    OrderId orderId = 1;

    for (auto _ : state) {
        state.PauseTiming();
        book.clear();
        state.ResumeTiming();

        // Add orders across multiple price levels
        for (int64_t i = 0; i < numLevels; ++i) {
            (void)book.addOrder(orderId++, Side::Buy, priceFromDouble(50000.0 - static_cast<double>(i)), 100);
        }
    }

    state.SetItemsProcessed(state.iterations() * numLevels);
}
BENCHMARK(BM_OrderBook_DeepBook_Add)->Range(10, 1000);

static void BM_OrderBook_GetTopLevels(benchmark::State& state) {
    DefaultOrderBook book(1);

    // Pre-populate with 100 levels
    for (OrderId i = 1; i <= 100; ++i) {
        (void)book.addOrder(i, Side::Buy, priceFromDouble(50000.0 - static_cast<double>(i)), 100);
        (void)book.addOrder(i + 100, Side::Sell, priceFromDouble(50000.0 + static_cast<double>(i)), 100);
    }

    std::vector<std::pair<Price, Quantity>> levels;
    levels.reserve(10);

    for (auto _ : state) {
        book.getTopLevels(Side::Buy, 10, levels);
        benchmark::DoNotOptimize(levels);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBook_GetTopLevels);

static void BM_OrderBook_RandomOperations(benchmark::State& state) {
    DefaultOrderBook book(1);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> opDist(0, 2);
    std::uniform_real_distribution<double> priceDist(49000.0, 51000.0);
    std::uniform_int_distribution<int> qtyDist(1, 1000);

    OrderId nextId = 1;
    std::vector<OrderId> activeOrders;
    activeOrders.reserve(10000);

    for (auto _ : state) {
        int op = opDist(rng);

        if (op == 0 || activeOrders.empty()) {
            // Add order
            Price price = priceFromDouble(priceDist(rng));
            Quantity qty = qtyDist(rng);
            Side side = (rng() % 2) ? Side::Buy : Side::Sell;

            if (book.addOrder(nextId, side, price, qty)) {
                activeOrders.push_back(nextId);
            }
            ++nextId;
        } else if (op == 1 && !activeOrders.empty()) {
            // Modify order
            std::size_t idx = rng() % activeOrders.size();
            (void)book.modifyOrder(activeOrders[idx], qtyDist(rng));
        } else if (!activeOrders.empty()) {
            // Delete order
            std::size_t idx = rng() % activeOrders.size();
            (void)book.deleteOrder(activeOrders[idx]);
            activeOrders.erase(activeOrders.begin() + static_cast<std::ptrdiff_t>(idx));
        }

        // Prevent unbounded growth
        if (activeOrders.size() > 5000) {
            for (std::size_t i = 0; i < 1000 && !activeOrders.empty(); ++i) {
                (void)book.deleteOrder(activeOrders.back());
                activeOrders.pop_back();
            }
        }
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderBook_RandomOperations);

BENCHMARK_MAIN();

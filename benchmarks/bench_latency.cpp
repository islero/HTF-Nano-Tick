/**
 * @file bench_latency.cpp
 * @brief Latency measurement benchmarks for HFT components.
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#include <benchmark/benchmark.h>
#include <core/Timestamp.hpp>
#include <core/MemoryArena.hpp>
#include <market_data/QuickFixApplication.hpp>
#include <orderbook/OrderBook.hpp>
#include <strategy/StrategyEngine.hpp>

#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/MarketDataSnapshotFullRefresh.h>
#include <quickfix/fix44/ExecutionReport.h>

using namespace hft;

//==============================================================================
// Timestamp Benchmarks
//==============================================================================

static void BM_NowNanos(benchmark::State& state) {
    for (auto _ : state) {
        Timestamp ts = nowNanos();
        benchmark::DoNotOptimize(ts);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NowNanos);

static void BM_SteadyNanos(benchmark::State& state) {
    for (auto _ : state) {
        auto ts = steadyNanos();
        benchmark::DoNotOptimize(ts);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SteadyNanos);

static void BM_RDTSC(benchmark::State& state) {
    for (auto _ : state) {
        auto tsc = rdtsc();
        benchmark::DoNotOptimize(tsc);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RDTSC);

static void BM_RDTSC_Fenced(benchmark::State& state) {
    for (auto _ : state) {
        auto tsc = rdtscFenced();
        benchmark::DoNotOptimize(tsc);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RDTSC_Fenced);

//==============================================================================
// Latency Histogram Benchmarks
//==============================================================================

static void BM_LatencyHistogram_Record(benchmark::State& state) {
    DefaultLatencyHistogram histogram;

    for (auto _ : state) {
        histogram.record(1000); // 1 microsecond
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LatencyHistogram_Record);

static void BM_LatencyHistogram_RecordCycles(benchmark::State& state) {
    DefaultLatencyHistogram histogram;

    for (auto _ : state) {
        auto start = rdtsc();
        // Simulate minimal work
        benchmark::ClobberMemory();
        auto end = rdtsc();
        histogram.recordCycles(start, end);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LatencyHistogram_RecordCycles);

static void BM_LatencyHistogram_Percentile(benchmark::State& state) {
    DefaultLatencyHistogram histogram;

    // Pre-populate
    for (int i = 0; i < 10000; ++i) {
        histogram.record(i * 100);
    }

    for (auto _ : state) {
        auto p50 = histogram.p50();
        auto p99 = histogram.p99();
        benchmark::DoNotOptimize(p50);
        benchmark::DoNotOptimize(p99);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LatencyHistogram_Percentile);

//==============================================================================
// Memory Arena Benchmarks
//==============================================================================

static void BM_LinearArena_Allocate(benchmark::State& state) {
    LinearArena<65536> arena;

    for (auto _ : state) {
        void* ptr = arena.allocate<8>(64);
        benchmark::DoNotOptimize(ptr);

        if (arena.remaining() < 128) {
            arena.reset();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LinearArena_Allocate);

static void BM_LinearArena_Create(benchmark::State& state) {
    LinearArena<65536> arena;

    for (auto _ : state) {
        Order* order = arena.create<Order>(1, priceFromDouble(100.0), 100, Side::Buy);
        benchmark::DoNotOptimize(order);

        if (arena.remaining() < sizeof(Order) * 2) {
            arena.reset();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LinearArena_Create);

static void BM_ObjectPool_Allocate(benchmark::State& state) {
    ObjectPool<Order, 10000> pool;
    std::vector<Order*> allocated;
    allocated.reserve(1000);

    for (auto _ : state) {
        Order* order = pool.allocate(1, priceFromDouble(100.0), 100, Side::Buy);
        benchmark::DoNotOptimize(order);

        if (order) {
            allocated.push_back(order);
        }

        // Return some orders periodically
        if (allocated.size() > 500) {
            for (std::size_t i = 0; i < 250; ++i) {
                pool.deallocate(allocated.back());
                allocated.pop_back();
            }
        }
    }

    // Cleanup
    for (auto* o : allocated) {
        pool.deallocate(o);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPool_Allocate);

static void BM_ObjectPool_AllocateDeallocate(benchmark::State& state) {
    ObjectPool<Order, 1000> pool;

    for (auto _ : state) {
        Order* order = pool.allocate(1, priceFromDouble(100.0), 100, Side::Buy);
        benchmark::DoNotOptimize(order);
        pool.deallocate(order);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_ObjectPool_AllocateDeallocate);

//==============================================================================
// QuickFIX Message Benchmarks
//==============================================================================

static void BM_QuickFix_BuildNewOrderSingle(benchmark::State& state) {
    for (auto _ : state) {
        FIX44::NewOrderSingle order;

        order.set(FIX::ClOrdID("12345"));
        order.set(FIX::Symbol("BTCUSD"));
        order.set(FIX::Side(qfix::SIDE_BUY));
        order.set(FIX::TransactTime(FIX::UtcTimeStamp()));
        order.set(FIX::OrdType(qfix::ORD_TYPE_LIMIT));
        order.set(FIX::Price(50000.50));
        order.set(FIX::OrderQty(100));
        order.set(FIX::TimeInForce(qfix::TIF_DAY));

        benchmark::DoNotOptimize(order);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuickFix_BuildNewOrderSingle);

static void BM_QuickFix_BuildAndSerialize(benchmark::State& state) {
    for (auto _ : state) {
        FIX44::NewOrderSingle order;

        order.set(FIX::ClOrdID("12345"));
        order.set(FIX::Symbol("BTCUSD"));
        order.set(FIX::Side(qfix::SIDE_BUY));
        order.set(FIX::TransactTime(FIX::UtcTimeStamp()));
        order.set(FIX::OrdType(qfix::ORD_TYPE_LIMIT));
        order.set(FIX::Price(50000.50));
        order.set(FIX::OrderQty(100));

        std::string msg = order.toString();
        benchmark::DoNotOptimize(msg);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuickFix_BuildAndSerialize);

static void BM_QuickFix_GetField(benchmark::State& state) {
    FIX44::NewOrderSingle order;
    order.set(FIX::ClOrdID("12345"));
    order.set(FIX::Symbol("BTCUSD"));
    order.set(FIX::Side(qfix::SIDE_BUY));
    order.set(FIX::TransactTime(FIX::UtcTimeStamp()));
    order.set(FIX::OrdType(qfix::ORD_TYPE_LIMIT));
    order.set(FIX::Price(50000.50));
    order.set(FIX::OrderQty(100));

    for (auto _ : state) {
        FIX::Symbol symbol;
        FIX::Price price;
        FIX::OrderQty qty;

        order.get(symbol);
        order.get(price);
        order.get(qty);

        benchmark::DoNotOptimize(symbol);
        benchmark::DoNotOptimize(price);
        benchmark::DoNotOptimize(qty);
    }
    state.SetItemsProcessed(state.iterations() * 3);
}
BENCHMARK(BM_QuickFix_GetField);

static void BM_QuickFix_MarketDataSnapshot(benchmark::State& state) {
    for (auto _ : state) {
        FIX44::MarketDataSnapshotFullRefresh snapshot;

        snapshot.set(FIX::Symbol("BTCUSD"));

        // Add 5 bid levels
        for (int i = 0; i < 5; ++i) {
            FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
            group.set(FIX::MDEntryType(qfix::MD_ENTRY_BID));
            group.set(FIX::MDEntryPx(50000.0 - i * 10));
            group.set(FIX::MDEntrySize(100 + i * 10));
            snapshot.addGroup(group);
        }

        // Add 5 ask levels
        for (int i = 0; i < 5; ++i) {
            FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
            group.set(FIX::MDEntryType(qfix::MD_ENTRY_ASK));
            group.set(FIX::MDEntryPx(50010.0 + i * 10));
            group.set(FIX::MDEntrySize(100 + i * 10));
            snapshot.addGroup(group);
        }

        benchmark::DoNotOptimize(snapshot);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuickFix_MarketDataSnapshot);

static void BM_QuickFix_ParseMarketDataEntries(benchmark::State& state) {
    // Pre-build snapshot
    FIX44::MarketDataSnapshotFullRefresh snapshot;
    snapshot.set(FIX::Symbol("BTCUSD"));

    for (int i = 0; i < 10; ++i) {
        FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
        group.set(FIX::MDEntryType(i < 5 ? qfix::MD_ENTRY_BID : qfix::MD_ENTRY_ASK));
        group.set(FIX::MDEntryPx(50000.0 + i * 10));
        group.set(FIX::MDEntrySize(100));
        snapshot.addGroup(group);
    }

    for (auto _ : state) {
        int numEntries = static_cast<int>(snapshot.groupCount(FIX::FIELD::NoMDEntries));
        Price totalPrice = 0;
        Quantity totalQty = 0;

        for (int i = 1; i <= numEntries; ++i) {
            FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
            snapshot.getGroup(static_cast<unsigned int>(i), group);

            FIX::MDEntryPx px;
            FIX::MDEntrySize sz;
            group.get(px);
            group.get(sz);

            totalPrice += priceFromDouble(px.getValue());
            totalQty += static_cast<Quantity>(sz.getValue());
        }

        benchmark::DoNotOptimize(totalPrice);
        benchmark::DoNotOptimize(totalQty);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuickFix_ParseMarketDataEntries);

static void BM_QuickFix_ExecutionReport(benchmark::State& state) {
    for (auto _ : state) {
        FIX44::ExecutionReport report;

        report.set(FIX::ClOrdID("12345"));
        report.set(FIX::OrderID("EX999"));
        report.set(FIX::ExecID("EXEC001"));
        report.set(FIX::ExecType(qfix::EXEC_TYPE_FILL));
        report.set(FIX::OrdStatus('2'));
        report.set(FIX::Symbol("BTCUSD"));
        report.set(FIX::Side(qfix::SIDE_BUY));
        report.set(FIX::LastPx(50000.50));
        report.set(FIX::LastQty(100));
        report.set(FIX::LeavesQty(0));
        report.set(FIX::CumQty(100));

        benchmark::DoNotOptimize(report);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuickFix_ExecutionReport);

//==============================================================================
// End-to-End Latency Simulation
//==============================================================================

static void BM_TickToSignal_Latency(benchmark::State& state) {
    // Setup order books
    DefaultOrderBook spotBook(1);
    DefaultOrderBook futuresBook(2);

    spotBook.addOrder(1, Side::Buy, priceFromDouble(50000.0), 100);
    spotBook.addOrder(2, Side::Sell, priceFromDouble(50010.0), 100);
    futuresBook.addOrder(3, Side::Buy, priceFromDouble(50100.0), 100);
    futuresBook.addOrder(4, Side::Sell, priceFromDouble(50110.0), 100);

    CashCarryConfig config;
    config.entryThreshold = priceFromDouble(50.0);
    CashCarryArbitrage strategy(&spotBook, &futuresBook, config, 1);
    strategy.initialize();

    OrderBookUpdate update;
    update.action = MdMsgType::Modify;
    update.side = Side::Buy;

    DefaultLatencyHistogram histogram;

    for (auto _ : state) {
        auto start = rdtsc();

        auto orders = strategy.onMarketData(update);
        benchmark::DoNotOptimize(orders);

        auto end = rdtsc();
        histogram.recordCycles(start, end);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["p50_ns"] = static_cast<double>(histogram.p50());
    state.counters["p99_ns"] = static_cast<double>(histogram.p99());
    state.counters["max_ns"] = static_cast<double>(histogram.max());
}
BENCHMARK(BM_TickToSignal_Latency);

static void BM_OrderBook_UpdatePath(benchmark::State& state) {
    DefaultOrderBook book(1);

    // Pre-populate
    for (OrderId i = 1; i <= 100; ++i) {
        book.addOrder(i, Side::Buy, priceFromDouble(50000.0 - static_cast<double>(i)), 100);
        book.addOrder(i + 100, Side::Sell, priceFromDouble(50000.0 + static_cast<double>(i)), 100);
    }

    DefaultLatencyHistogram histogram;
    OrderId nextId = 201;

    for (auto _ : state) {
        auto start = rdtsc();

        // Add order
        book.addOrder(nextId, Side::Buy, priceFromDouble(49950.0), 50);

        // Query BBO
        auto bid = book.bestBid();
        auto ask = book.bestAsk();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);

        // Delete order
        book.deleteOrder(nextId);

        auto end = rdtsc();
        histogram.recordCycles(start, end);
        ++nextId;
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["p50_ns"] = static_cast<double>(histogram.p50());
    state.counters["p99_ns"] = static_cast<double>(histogram.p99());
}
BENCHMARK(BM_OrderBook_UpdatePath);

//==============================================================================
// Price Conversion Benchmarks
//==============================================================================

static void BM_PriceFromDouble(benchmark::State& state) {
    double prices[] = {100.5, 50000.12345678, 0.00000001, 99999.99999999};
    std::size_t idx = 0;

    for (auto _ : state) {
        Price p = priceFromDouble(prices[idx]);
        benchmark::DoNotOptimize(p);
        idx = (idx + 1) % 4;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PriceFromDouble);

static void BM_PriceToDouble(benchmark::State& state) {
    Price prices[] = {priceFromDouble(100.5), priceFromDouble(50000.12345678), priceFromDouble(0.00000001),
                      priceFromDouble(99999.99999999)};
    std::size_t idx = 0;

    for (auto _ : state) {
        double d = priceToDouble(prices[idx]);
        benchmark::DoNotOptimize(d);
        idx = (idx + 1) % 4;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PriceToDouble);

//==============================================================================
// QuickFIX Type Conversion Benchmarks
//==============================================================================

static void BM_QuickFix_PriceConversion(benchmark::State& state) {
    FIX::Price fixPrice(50000.12345678);

    for (auto _ : state) {
        double d = fixPrice.getValue();
        Price p = priceFromDouble(d);
        benchmark::DoNotOptimize(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuickFix_PriceConversion);

static void BM_QuickFix_UtcTimestamp(benchmark::State& state) {
    for (auto _ : state) {
        FIX::UtcTimeStamp ts;
        benchmark::DoNotOptimize(ts);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QuickFix_UtcTimestamp);

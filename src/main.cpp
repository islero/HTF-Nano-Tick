/**
 * @file main.cpp
 * @brief Example application demonstrating the HFT NanoTick framework.
 *
 * This example shows how to wire together the components of an HFT system:
 * - Market data handling with stale data protection
 * - Order book management
 * - Cash-and-Carry arbitrage strategy
 * - Order entry gateway
 *
 * @author HFT NanoTick Team
 * @copyright MIT License
 */

#include <core/Types.hpp>
#include <core/SPSCQueue.hpp>
#include <core/Timestamp.hpp>
#include <core/MemoryArena.hpp>
#include <orderbook/OrderBook.hpp>
#include <strategy/StrategyEngine.hpp>
// #include <gateway/OrderEntryGateway.hpp> // Disabled - requires QuickFIX
// #include <market_data/MarketDataHandler.hpp> // Disabled - requires QuickFIX
#include <utils/Logger.hpp>

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>

using namespace hft;

/**
 * @brief Demonstrate the order book functionality.
 */
void demonstrateOrderBook() {
    std::cout << "\n=== Order Book Demo ===\n";

    DefaultOrderBook book(1); // Symbol ID 1

    // Add some orders
    (void)book.addOrder(1, Side::Buy, priceFromDouble(100.50), 100);
    (void)book.addOrder(2, Side::Buy, priceFromDouble(100.45), 200);
    (void)book.addOrder(3, Side::Buy, priceFromDouble(100.40), 150);

    (void)book.addOrder(4, Side::Sell, priceFromDouble(100.55), 100);
    (void)book.addOrder(5, Side::Sell, priceFromDouble(100.60), 200);
    (void)book.addOrder(6, Side::Sell, priceFromDouble(100.65), 150);

    std::cout << "Best Bid: " << priceToDouble(book.bestBid()) << " @ " << book.bestBidQty() << "\n";
    std::cout << "Best Ask: " << priceToDouble(book.bestAsk()) << " @ " << book.bestAskQty() << "\n";
    std::cout << "Mid Price: " << priceToDouble(book.midPrice()) << "\n";
    std::cout << "Spread: " << priceToDouble(book.spread()) << "\n";
    std::cout << "Is Crossed: " << (book.isCrossed() ? "Yes" : "No") << "\n";
    std::cout << "Order Count: " << book.orderCount() << "\n";

    // Modify an order
    (void)book.modifyOrder(1, 50);
    std::cout << "\nAfter modifying order 1 to qty 50:\n";
    std::cout << "Best Bid Qty: " << book.bestBidQty() << "\n";

    // Delete an order
    (void)book.deleteOrder(4);
    std::cout << "\nAfter deleting order 4:\n";
    std::cout << "Best Ask: " << priceToDouble(book.bestAsk()) << "\n";
}

/**
 * @brief Demonstrate the SPSC queue performance.
 */
void demonstrateSPSCQueue() {
    std::cout << "\n=== SPSC Queue Demo ===\n";

    SPSCQueue<int, 1024> queue;

    // Single-threaded test
    for (int i = 0; i < 10; ++i) {
        if (!queue.tryPush(i)) {
            std::cout << "Failed to push " << i << "\n";
        }
    }

    std::cout << "Queue size: " << queue.sizeApprox() << "\n";

    int value;
    while (queue.tryPop(value)) {
        std::cout << "Popped: " << value << " ";
    }
    std::cout << "\n";

    // Performance test
    constexpr int NUM_ITERATIONS = 1'000'000;

    auto start = std::chrono::high_resolution_clock::now();

    std::thread producer([&queue]() {
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            while (!queue.tryPush(i)) {
                // Spin
            }
        }
    });

    std::thread consumer([&queue]() {
        int val;
        for (int i = 0; i < NUM_ITERATIONS; ++i) {
            while (!queue.tryPop(val)) {
                // Spin
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    double nsPerOp = static_cast<double>(duration.count()) / NUM_ITERATIONS;
    double opsPerSec = 1e9 / nsPerOp;

    std::cout << "SPSC Queue Performance:\n";
    std::cout << "  " << NUM_ITERATIONS << " push/pop pairs\n";
    std::cout << "  " << std::fixed << std::setprecision(1) << nsPerOp << " ns/op\n";
    std::cout << "  " << std::fixed << std::setprecision(0) << opsPerSec << " ops/sec\n";
}

/**
 * @brief Demonstrate the latency histogram.
 */
void demonstrateLatencyHistogram() {
    std::cout << "\n=== Latency Histogram Demo ===\n";

    DefaultLatencyHistogram histogram;

    // Simulate some latency measurements
    for (int i = 0; i < 10000; ++i) {
        // Measure time for a simple operation
        auto start = rdtsc();

        // Simulate some work (prevent optimization)
        int sum = 0;
        for (int j = 0; j < 100; ++j) {
            sum += j;
        }
        (void)sum;

        auto end = rdtsc();
        histogram.recordCycles(start, end);
    }

    std::cout << "Latency Statistics (nanoseconds):\n";
    std::cout << "  Min:  " << histogram.min() << " ns\n";
    std::cout << "  P50:  " << histogram.p50() << " ns\n";
    std::cout << "  P90:  " << histogram.p90() << " ns\n";
    std::cout << "  P99:  " << histogram.p99() << " ns\n";
    std::cout << "  P999: " << histogram.p999() << " ns\n";
    std::cout << "  Max:  " << histogram.max() << " ns\n";
    std::cout << "  Avg:  " << std::fixed << std::setprecision(1) << histogram.avg() << " ns\n";
    std::cout << "  Count: " << histogram.count() << "\n";
}

/**
 * @brief Demonstrate the Cash-and-Carry arbitrage strategy.
 */
void demonstrateStrategy() {
    std::cout << "\n=== Cash-and-Carry Arbitrage Strategy Demo ===\n";

    // Create order books for spot and futures
    DefaultOrderBook spotBook(1); // BTCUSD Spot
    DefaultOrderBook futuresBook(2); // BTCUSD Futures

    // Setup initial market state
    // Spot market
    (void)spotBook.addOrder(1, Side::Buy, priceFromDouble(50000.00), 10);
    (void)spotBook.addOrder(2, Side::Sell, priceFromDouble(50010.00), 10);

    // Futures market (trading at premium)
    (void)futuresBook.addOrder(3, Side::Buy, priceFromDouble(50100.00), 10);
    (void)futuresBook.addOrder(4, Side::Sell, priceFromDouble(50110.00), 10);

    std::cout << "Initial Market State:\n";
    std::cout << "  Spot:    " << priceToDouble(spotBook.bestBid()) << " / " << priceToDouble(spotBook.bestAsk())
              << "\n";
    std::cout << "  Futures: " << priceToDouble(futuresBook.bestBid()) << " / " << priceToDouble(futuresBook.bestAsk())
              << "\n";

    // Create strategy with custom threshold
    CashCarryConfig config;
    config.entryThreshold = priceFromDouble(50.0); // $50 minimum spread
    config.defaultQty = 1;

    CashCarryArbitrage strategy(&spotBook, &futuresBook, config, 1);

    // Initialize and run strategy
    if (!strategy.initialize()) {
        std::cout << "Failed to initialize strategy\n";
        return;
    }

    std::cout << "\nStrategy initialized. State: " << (strategy.isActive() ? "Active" : "Inactive") << "\n";

    // Simulate a market data tick
    OrderBookUpdate update;
    update.action = MdMsgType::Add;
    update.side = Side::Sell;
    update.price = spotBook.bestAsk();
    update.quantity = 10;

    auto orders = strategy.onMarketData(update);

    std::cout << "\nBasis: $" << priceToDouble(strategy.currentBasis()) << "\n";
    std::cout << "Threshold: $" << priceToDouble(config.entryThreshold) << "\n";

    if (orders.empty()) {
        std::cout << "No trading signal (basis below threshold)\n";
    } else {
        std::cout << "\nGenerated " << orders.size() << " orders:\n";
        for (const auto& order : orders) {
            std::cout << "  Symbol: " << order.symbolId << ", Side: " << (order.side == Side::Buy ? "BUY" : "SELL")
                      << ", Price: " << priceToDouble(order.price) << ", Qty: " << order.quantity << "\n";
        }
    }

    // Increase futures premium to trigger signal
    std::cout << "\n--- Increasing futures premium ---\n";
    (void)futuresBook.addOrder(5, Side::Buy, priceFromDouble(50120.00), 10);

    std::cout << "New Futures Bid: " << priceToDouble(futuresBook.bestBid()) << "\n";
    std::cout << "New Basis: $" << priceToDouble(futuresBook.bestBid() - spotBook.bestAsk()) << "\n";

    orders = strategy.onMarketData(update);

    if (!orders.empty()) {
        std::cout << "\nArbitrage opportunity detected!\n";
        std::cout << "Generated " << orders.size() << " orders:\n";
        for (const auto& order : orders) {
            std::cout << "  Symbol: " << order.symbolId << ", Side: " << (order.side == Side::Buy ? "BUY" : "SELL")
                      << ", Price: $" << priceToDouble(order.price) << ", Qty: " << order.quantity << "\n";
        }
    }

    std::cout << "\nStrategy Statistics:\n";
    std::cout << "  Ticks Processed: " << strategy.stats().ticksProcessed.load() << "\n";
    std::cout << "  Signals Generated: " << strategy.stats().signalsGenerated.load() << "\n";
}

/**
 * @brief Demonstrate the memory arena.
 */
void demonstrateMemoryArena() {
    std::cout << "\n=== Memory Arena Demo ===\n";

    // Linear arena for scratch allocations
    LinearArena<4096> arena;

    std::cout << "Arena capacity: " << arena.capacity() << " bytes\n";
    std::cout << "Used: " << arena.used() << " bytes\n";

    // Allocate some objects
    auto* price1 = arena.create<Price>(priceFromDouble(100.50));
    auto* price2 = arena.create<Price>(priceFromDouble(200.75));

    std::cout << "After allocating 2 prices:\n";
    std::cout << "  Used: " << arena.used() << " bytes\n";
    std::cout << "  Price1: " << priceToDouble(*price1) << "\n";
    std::cout << "  Price2: " << priceToDouble(*price2) << "\n";

    arena.reset();
    std::cout << "After reset: " << arena.used() << " bytes\n";

    // Object pool demo
    ObjectPool<Order, 100> orderPool;

    std::cout << "\nObject Pool:\n";
    std::cout << "  Capacity: " << orderPool.capacity() << "\n";
    std::cout << "  Available: " << orderPool.availableCount() << "\n";

    auto* order1 = orderPool.allocate(1ULL, priceFromDouble(100.0), 100, Side::Buy);
    auto* order2 = orderPool.allocate(2ULL, priceFromDouble(101.0), 200, Side::Sell);

    std::cout << "After allocating 2 orders:\n";
    std::cout << "  Allocated: " << orderPool.allocatedCount() << "\n";
    std::cout << "  Available: " << orderPool.availableCount() << "\n";

    orderPool.deallocate(order1);
    std::cout << "After deallocating 1 order:\n";
    std::cout << "  Allocated: " << orderPool.allocatedCount() << "\n";

    orderPool.deallocate(order2);
}

/**
 * @brief Main entry point.
 */
int main() {
    std::cout << "====================================\n";
    std::cout << " HFT NanoTick Framework Demo\n";
    std::cout << "====================================\n";

    // Initialize logging
    auto& logger = getLogger();
    logger.setLevel(LogLevel::Info);
    (void)logger.info("HFT NanoTick starting...");

    try {
        demonstrateOrderBook();
        demonstrateSPSCQueue();
        demonstrateLatencyHistogram();
        demonstrateStrategy();
        demonstrateMemoryArena();
    } catch (const std::exception& e) {
        (void)logger.error("Exception caught");
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    (void)logger.info("HFT NanoTick demo completed");
    logger.flush();

    std::cout << "\n====================================\n";
    std::cout << " Demo Complete\n";
    std::cout << "====================================\n";

    return 0;
}

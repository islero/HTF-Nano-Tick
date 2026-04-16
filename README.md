# HFT NanoTick

[![CI](https://github.com/islero/HTF-Nano-Tick/actions/workflows/ci.yml/badge.svg)](https://github.com/islero/HTF-Nano-Tick/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)]()

A low-latency trading engine prototype implemented in modern C++20. The project demonstrates bounded order-book storage, lock-free SPSC queues, protocol-neutral order submission, and a **Spot-Futures Cash and Carry Arbitrage Strategy**. It is a technical portfolio/learning system, not a complete production trading stack.

## Highlights

- **Bounded order book**: Preallocated order slots, price levels, and open-addressed order-id lookup
- **Allocation-aware hot paths**: No heap allocation for order book add/modify/delete, SPSC queue push/pop, or strategy batch output
- **Lock-free concurrency**: SPSC queues with wait-free guarantees
- **Compile-time optimization**: CRTP eliminates virtual dispatch overhead
- **Protocol-neutral gateway**: Core order lifecycle no longer depends on QuickFIX types
- **Cross-platform CI intent**: Linux, macOS, Windows, sanitizers, formatting, and static analysis
- **Production patterns**: Cache-line alignment, false sharing prevention, RDTSC timing

## Table of Contents

- [Highlights](#highlights)
- [Architecture Overview](#architecture-overview)
- [Project Structure](#project-structure)
- [Design Philosophy](#design-philosophy)
- [Key Components](#key-components)
- [Build Instructions](#build-instructions)
- [Running Tests & Benchmarks](#running-tests--benchmarks)
- [Performance Considerations](#performance-considerations)
- [Design Decisions](#design-decisions)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              HFT NanoTick Architecture                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────┐    ┌──────────────────┐    ┌─────────────────────┐     │
│  │  Protocol       │───▶│ Market Data      │───▶│    Order Book       │     │
│  │  Adapter        │    │ Handler          │    │    (Spot/Futures)   │     │
│  └─────────────────┘    │ • Stale Check    │    └──────────┬──────────┘     │
│                         │ • Seq Gap Detect │               │                │
│                         └──────────────────┘               │                │
│                                                            ▼                │
│                         ┌──────────────────┐    ┌─────────────────────┐     │
│                         │  SPSC Queue      │◀───│   Strategy Engine   │     │
│                         │  (Lock-Free)     │    │   (CRTP Pattern)    │     │
│                         └────────┬─────────┘    │   Cash & Carry Arb  │     │
│                                  │              └─────────────────────┘     │
│                                  ▼                                          │
│                         ┌──────────────────┐    ┌─────────────────────┐     │
│                         │  Order Entry     │───▶│   Risk Management   │     │
│                         │  Gateway         │    │   • Rate Limiting   │     │
│                         │  • Native Msgs   │    │   • Pending Exposure│     │
│                         └──────────────────┘    └─────────────────────┘     │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                        Support Infrastructure                       │    │
│  │  • Async Logger (Lock-Free)  • Latency Histogram  • Memory Arena    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Data Flow

1. **Market Data Ingestion**: Protocol adapters normalize wire messages into `MarketDataMessage`
2. **Stale Data Protection**: Messages older than threshold (default 50ms) are discarded
3. **Order Book Update**: Limit order book is updated with price/quantity changes
4. **Strategy Evaluation**: CRTP-based strategy engine evaluates arbitrage opportunities
5. **Order Generation**: Paired orders (buy spot/sell futures or vice versa) are created
6. **Risk Validation**: Pre-trade risk checks (position limits, rate limits) are applied
7. **Order Submission**: Protocol-neutral outbound order messages are emitted for an adapter or simulator

---

## Project Structure

```
HFT NanoTick/
├── CMakeLists.txt              # Modern CMake build configuration
├── README.md                   # This documentation
│
├── include/                    # Header-only library
│   ├── core/
│   │   ├── Types.hpp          # Fundamental types, price representation
│   │   ├── SPSCQueue.hpp      # Lock-free single-producer single-consumer queue
│   │   ├── MemoryArena.hpp    # Object pools and linear allocators
│   │   └── Timestamp.hpp      # RDTSC timing and latency histograms
│   │
│   ├── market_data/
│   │   ├── MarketDataHandler.hpp  # Stale data protection, sequence gaps
│   │   └── QuickFixApplication.hpp # Optional QuickFIX integration, disabled by default
│   │
│   ├── orderbook/
│   │   └── OrderBook.hpp      # Templated limit order book
│   │
│   ├── strategy/
│   │   └── StrategyEngine.hpp # CRTP strategy framework, Cash & Carry
│   │
│   ├── gateway/
│   │   └── OrderEntryGateway.hpp  # Order management, risk checks, outbound commands
│   │
│   └── utils/
│       └── Logger.hpp         # Async lock-free logging
│
├── src/
│   └── main.cpp               # Example application
│
├── tests/                     # Google Test unit tests
│   ├── test_orderbook.cpp
│   ├── test_gateway.cpp
│   ├── test_latency.cpp
│   ├── test_public_headers.cpp
│   ├── test_spsc_queue.cpp
│   └── test_strategy.cpp
│
└── benchmarks/                # Google Benchmark performance tests
    ├── bench_orderbook.cpp
    ├── bench_spsc_queue.cpp
    └── bench_latency.cpp
```

---

## Design Philosophy

### Zero-Allocation Hot Path

In HFT, memory allocation is a significant source of latency variance:
- **malloc/new** can take 100+ nanoseconds and cause page faults
- Dynamic allocation invokes the system allocator, potentially blocking

**Our approach:**
- Pre-allocated order slots, price levels, and open-addressed order-id indexes
- Linear arenas for per-tick scratch memory
- Fixed-capacity strategy order batches
- Bounded ring buffer for gateway rate limiting

### Cache Efficiency

Modern CPUs execute faster than they can fetch data:
- L1 cache: ~4 cycles (~1.3ns)
- L2 cache: ~12 cycles (~4ns)
- L3 cache: ~40 cycles (~13ns)
- Main memory: ~200+ cycles (~70ns)

**Our approach:**
- `alignas(64)` on frequently accessed structures to prevent false sharing
- Contiguous bounded arrays in order books
- SPSC queue slots aligned to cache lines
- Hot/cold data separation

### Lock-Free Concurrency

Traditional locks introduce:
- Context switches (1000+ cycles)
- Priority inversion risks
- Deadlock potential

**Our approach:**
- SPSC queues for producer-consumer patterns
- Atomic operations with appropriate memory ordering
- Wait-free algorithms where possible

---

## Key Components

### SPSC Queue (`SPSCQueue.hpp`)

Lock-free single-producer single-consumer queue using Lamport's algorithm:

```cpp
SPSCQueue<MarketDataMessage, 65536> queue;

// Producer thread
queue.tryPush(message);

// Consumer thread
MarketDataMessage msg;
if (queue.tryPop(msg)) {
    processMessage(msg);
}
```

**Key features:**
- Power-of-two capacity for fast modulo
- Acquire-release memory ordering
- Cache-line padding between head/tail
- Wait-free progress guarantee

### Order Book (`OrderBook.hpp`)

Templated limit order book with O(1) best bid/ask access:

```cpp
DefaultOrderBook book(symbolId);

// Add orders
book.addOrder(orderId, Side::Buy, price, quantity);

// Query BBO
Price bestBid = book.bestBid();
Price bestAsk = book.bestAsk();
Price spread = book.spread();

// Modify/Delete
book.modifyOrder(orderId, newQuantity);
book.deleteOrder(orderId);
```

**Key features:**
- Preallocated order slots, reusable through a bounded free list
- Sorted bounded price-level arrays per side
- Open-addressed order-id lookup
- Intrusive FIFO links per price level
- Callback support for market data updates

### CRTP Strategy Engine (`StrategyEngine.hpp`)

Compile-time polymorphism eliminates virtual function overhead:

```cpp
class MyStrategy : public StrategyBase<MyStrategy> {
public:
    Signal computeSignal() noexcept {
        // Strategy logic inlined at compile time
        if (spread > threshold) {
            return Signal::BuySpot;
        }
        return Signal::None;
    }
};
```

**Why CRTP over virtual functions:**
- vtable lookup: ~3-4 cycles + potential cache miss
- CRTP: Zero overhead, fully inlined
- Enables aggressive compiler optimizations

### Strategy Engine (`StrategyEngine.hpp`)

The strategy API has a fixed-capacity output path for latency-sensitive code:

```cpp
CashCarryArbitrage strategy(&spotBook, &futureBook);
StrategyBase<CashCarryArbitrage>::OrderBatch orders;

std::size_t count = strategy.onMarketDataInto(update, orders);
for (std::size_t i = 0; i < count; ++i) {
    submit(orders[i]);
}
```

The older `onMarketData()` wrapper still returns `std::vector<OrderRequest>` for tests and examples, but production-style code should use `onMarketDataInto()`.

### Market Data Handler (`MarketDataHandler.hpp`)

Stale data protection prevents trading on outdated quotes:

```cpp
MarketDataConfig config;
config.staleThresholdNanos = 50'000'000; // 50ms

DefaultMarketDataHandler handler(config);

MarketDataMessage msg;
msg.symbolId = 1;
msg.sendingTime = exchangeTime;
msg.receiveTime = nowNanos();

handler.enqueue(msg);
handler.processNext();
```

**Stale data check logic:**
```
if (receiveTime - sendingTime > threshold) {
    discard message  // Don't trade on stale quotes
}
```

### Order Entry Gateway (`OrderEntryGateway.hpp`)

The gateway core emits protocol-neutral commands:

```cpp
DefaultOrderGateway gateway;
gateway.setSendCallback([](const OutboundOrderMessage& message) {
    // Translate to FIX, native binary protocol, or simulator input.
    return true;
});
```

Risk checks include max notional, open/pending order limits, rate limiting, and pending exposure in position-limit checks. QuickFIX-specific translation is intentionally outside the core gateway path unless `HFT_ENABLE_QUICKFIX` is provided by a separate integration build.

---

## Build Instructions

### Prerequisites

- C++20 compatible compiler:
  - GCC 10+
  - Clang 12+
  - MSVC 19.29+ (VS 2019 16.10+)
- CMake 3.20+
- Git (for fetching dependencies)

### Building

```bash
# Clone repository
git clone https://github.com/yourusername/hft-nanotick.git
cd hft-nanotick

# Create build directory
mkdir build && cd build

# Configure (Release build recommended for benchmarks)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build
cmake --build . -j$(nproc)
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `HFT_BUILD_TESTS` | ON | Build unit tests |
| `HFT_BUILD_BENCHMARKS` | ON | Build benchmarks |
| `HFT_ENABLE_SANITIZERS` | OFF | Enable ASan/UBSan |
| `HFT_ENABLE_LTO` | OFF | Link-time optimization |
| `HFT_NATIVE_ARCH` | ON | Optimize for native CPU |

```bash
# Example: Build with sanitizers for debugging
cmake -DHFT_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug ..
```

---

## Running Tests & Benchmarks

### Unit Tests

```bash
cd build
ctest --output-on-failure

# Or run directly
./hft_tests
```

### Benchmarks

```bash
./hft_benchmarks

# Run specific benchmark
./hft_benchmarks --benchmark_filter=BM_OrderBook

# Output JSON for analysis
./hft_benchmarks --benchmark_format=json > results.json
```

### Example Application

```bash
./hft_example
```

---

## Benchmark Results

Representative local short run on Apple M4 Pro, compiled with `-O3 -march=native`.
Google Benchmark could not set thread affinity on this machine, so treat these as local regression numbers, not exchange-grade latency claims.

### Order Book Performance

| Benchmark | Time | Throughput |
|-----------|------|------------|
| AddOrder | 50.8 ns | 19.7 M ops/s |
| AddAndDelete | 22.6 ns | 44.3 M ops/s |
| ModifyOrder | 3.11 ns | 321 M ops/s |
| RandomOperations | 61.2 ns | 16.3 M ops/s |

### SPSC Queue Performance

| Benchmark | Time | Throughput |
|-----------|------|------------|
| PushPop (single thread) | 3.14 ns | 637 M ops/s |
| Two-Thread Throughput | 24.8 ns | 40.3 M ops/s |

---

## Performance Considerations

### Target Latencies

| Component | Target | Notes |
|-----------|--------|-------|
| SPSC Queue Push/Pop | < 20 ns | Single cache line access |
| Order Book Add | < 500 ns | Bounded level insert and order-id index |
| Order Book BBO Query | < 10 ns | Cached values |
| Strategy Signal | < 100 ns | Use fixed-capacity output batch |
| Gateway Risk Check | < 500 ns | Includes pending exposure and rate-limit ring |
| Tick-to-Trade | TBD | Requires protocol adapter, replay harness, and end-to-end measurement |

### Optimization Checklist

- [ ] Compile with `-O3 -march=native`
- [ ] Pin threads to specific CPU cores
- [ ] Disable CPU frequency scaling
- [ ] Use huge pages for large allocations
- [ ] Isolate CPU cores from OS scheduler
- [ ] Consider kernel bypass (DPDK/RDMA) for networking

---

## Design Decisions

### Why CRTP for Strategies?

Virtual function dispatch involves:
1. Load vtable pointer from object
2. Index into vtable
3. Load function pointer
4. Indirect call

This chain causes:
- 2-3 pointer chasing operations
- Potential L1 cache misses
- Branch misprediction on indirect call

CRTP eliminates all of this by resolving at compile time.

### Why Lock-Free Queues?

Traditional mutex-based synchronization:
- `pthread_mutex_lock`: 20-30ns uncontended
- Context switch on contention: 1000+ cycles
- Introduces priority inversion risks

SPSC queue with atomics:
- Single atomic load/store: 10-20ns
- No system calls
- Wait-free progress

### Why Fixed-Point Prices?

Floating-point issues in finance:
- `0.1 + 0.2 ≠ 0.3` in IEEE 754
- Rounding errors accumulate
- Different results on different hardware

Fixed-point with 8 decimal places:
- Exact representation
- Integer arithmetic (faster)
- Deterministic results

### Why a Protocol-Neutral Gateway Core?

Order lifecycle, risk checks, and position tracking should not depend on a specific wire protocol library. The core gateway emits `OutboundOrderMessage`; a separate adapter can translate that to FIX, a binary native protocol, or a simulator.

Benefits:
- Public headers compile without QuickFIX installed
- Risk and order-state tests run without transport dependencies
- Exchange-specific serialization can evolve independently

---

## License

MIT License - See [LICENSE](LICENSE) for details.

---

## Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

---

## Disclaimer

This is an educational project demonstrating HFT architecture patterns. It is **not** intended for production trading without significant additional work including:
- Network layer (kernel bypass, multicast handling)
- Persistence and recovery
- Comprehensive risk management
- Regulatory compliance
- Thorough testing and certification

**Trading involves significant risk. Use at your own risk.**

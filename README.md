# HFT NanoTick

A high-frequency trading (HFT) system skeleton implemented in modern C++20, designed to demonstrate production-grade low-latency architecture patterns. This project implements a **Spot-Futures Cash and Carry Arbitrage Strategy** with emphasis on zero-allocation hot paths, lock-free data structures, and compile-time optimizations.

## Table of Contents

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
│  │  Network Layer  │───▶│ Market Data      │───▶│    Order Book       │     │
│  │  (FIX Protocol) │    │ Handler          │    │    (Spot/Futures)   │     │
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
│                         │  • FIX Builder   │    │   • Position Limits │     │
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

1. **Market Data Ingestion**: FIX messages are parsed using a zero-copy parser
2. **Stale Data Protection**: Messages older than threshold (default 50ms) are discarded
3. **Order Book Update**: Limit order book is updated with price/quantity changes
4. **Strategy Evaluation**: CRTP-based strategy engine evaluates arbitrage opportunities
5. **Order Generation**: Paired orders (buy spot/sell futures or vice versa) are created
6. **Risk Validation**: Pre-trade risk checks (position limits, rate limits) are applied
7. **Order Submission**: FIX messages are built and sent to the exchange

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
│   │   ├── FixParser.hpp      # Zero-copy FIX protocol parser
│   │   └── MarketDataHandler.hpp  # Stale data protection, sequence gaps
│   │
│   ├── orderbook/
│   │   └── OrderBook.hpp      # Templated limit order book
│   │
│   ├── strategy/
│   │   └── StrategyEngine.hpp # CRTP strategy framework, Cash & Carry
│   │
│   ├── gateway/
│   │   └── OrderEntryGateway.hpp  # Order management, risk checks
│   │
│   └── utils/
│       └── Logger.hpp         # Async lock-free logging
│
├── src/
│   └── main.cpp               # Example application
│
├── tests/                     # Google Test unit tests
│   ├── test_orderbook.cpp
│   ├── test_spsc_queue.cpp
│   ├── test_fix_parser.cpp
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
- Pre-allocated object pools for orders and messages
- Linear arenas for per-tick scratch memory
- Fixed-size buffers for FIX message building
- Stack allocation where possible

### Cache Efficiency

Modern CPUs execute faster than they can fetch data:
- L1 cache: ~4 cycles (~1.3ns)
- L2 cache: ~12 cycles (~4ns)
- L3 cache: ~40 cycles (~13ns)
- Main memory: ~200+ cycles (~70ns)

**Our approach:**
- `alignas(64)` on frequently accessed structures to prevent false sharing
- Contiguous memory layouts in order books
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
- std::map for price-time priority
- Hash map for O(1) order lookup by ID
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

### Market Data Handler (`MarketDataHandler.hpp`)

Stale data protection prevents trading on outdated quotes:

```cpp
MarketDataConfig config;
config.staleThresholdNanos = 50'000'000; // 50ms

DefaultMarketDataHandler handler(config);

// Process message - automatically filters stale data
handler.processMessage(data, length, receiveTime);
```

**Stale data check logic:**
```
if (receiveTime - sendingTime > threshold) {
    discard message  // Don't trade on stale quotes
}
```

### Zero-Copy FIX Parser (`FixParser.hpp`)

Parses FIX messages without memory allocation:

```cpp
FixParser parser;
if (parser.parse(data, length)) {
    auto symbol = parser.symbol();           // string_view into original buffer
    Price price = parser.getField(44).asPrice();
    int64_t qty = parser.getField(38).asInt();
}
```

**Design:**
- String views point into original message buffer
- Indexed tag lookup for common fields (O(1))
- Linear scan fallback for rare fields

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

Measured on Apple M4 Pro, compiled with `-O3 -march=native`:

### Order Book Performance

| Benchmark | Time | Throughput |
|-----------|------|------------|
| AddOrder | 37.4 ns | 26.7 M ops/s |
| AddAndDelete | 63.4 ns | 15.8 M ops/s |
| ModifyOrder | 13.7 ns | 72.9 M ops/s |
| BestBidQuery | 0.27 ns | 3.75 G ops/s |
| MidPriceQuery | 0.47 ns | 2.14 G ops/s |
| SpreadQuery | 0.27 ns | 3.74 G ops/s |
| GetTopLevels | 41.7 ns | 24.0 M ops/s |
| RandomOperations | 111 ns | 9.0 M ops/s |

**Deep Book Operations (batch add):**

| Orders | Time | Throughput |
|--------|------|------------|
| 10 | 870 ns | 11.5 M ops/s |
| 64 | 3.58 µs | 17.9 M ops/s |
| 512 | 30.7 µs | 16.7 M ops/s |
| 1000 | 62.4 µs | 16.0 M ops/s |

### SPSC Queue Performance

| Benchmark | Time | Throughput |
|-----------|------|------------|
| PushPop (single thread) | 3.09 ns | 647 M ops/s |
| Push (single thread) | 2.83 ns | 353 M ops/s |
| Emplace (single thread) | 2.84 ns | 352 M ops/s |
| Two-Thread Throughput | 49.3 ns | 20.3 M ops/s |
| Latency (avg) | 46.3 ns | 32.8 ns avg latency |
| FrontPeek | 0.31 ns | 3.28 G ops/s |
| EmptyCheck | 0.57 ns | 1.76 G ops/s |
| SizeApprox | 0.60 ns | 1.66 G ops/s |

**Element Size Scaling:**

| Size (bytes) | Time | Bandwidth |
|--------------|------|-----------|
| 8 | 2.68 ns | 5.6 GiB/s |
| 64 | 3.47 ns | 34.4 GiB/s |
| 128 | 4.44 ns | 53.7 GiB/s |
| 256 | 7.39 ns | 64.5 GiB/s |

**Queue Size Impact:**

| Capacity | Time | Throughput |
|----------|------|------------|
| 1K | 2.32 ns | 864 M ops/s |
| 64K | 2.72 ns | 734 M ops/s |

**Burst Operations:**

| Burst Size | Time | Throughput |
|------------|------|------------|
| 64 | 215 ns | 596 M ops/s |
| 512 | 2.26 µs | 454 M ops/s |
| 4096 | 19.4 µs | 422 M ops/s |

---

## Performance Considerations

### Target Latencies

| Component | Target | Notes |
|-----------|--------|-------|
| SPSC Queue Push/Pop | < 20 ns | Single cache line access |
| Order Book Add | < 500 ns | Map insertion |
| Order Book BBO Query | < 10 ns | Cached values |
| FIX Parse | < 100 ns | Zero-copy, no allocation |
| Strategy Signal | < 50 ns | Inlined via CRTP |
| Tick-to-Trade | < 1 µs | End-to-end target |

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

### Why Zero-Copy FIX Parsing?

Traditional parsing copies strings:
```cpp
std::string symbol = extractField(message, 55);  // Allocates!
```

Zero-copy approach:
```cpp
std::string_view symbol = getField(55).value;  // Just a pointer + length
```

Benefits:
- No heap allocation
- No string copying
- Cache-friendly linear scan

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

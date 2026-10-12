# System Architecture & Technical Design

This document details the low-latency design principles, memory layout guarantees, pre-trade risk controls, and internal component pipelines of `schwab-executor`.

---

## 1. Low-Latency Design Principles

The execution engine is engineered in C++20 for deterministic latency and zero jitter on execution paths:

### 1. Zero Dynamic Heap Allocation on Hot Paths
All domain entities on the execution path avoid heap allocations:
- **`Symbol`:** Stack-allocated fixed-size string buffer (`FixedString<15>`) avoiding `std::string` allocations.
- **`StrategyTag`:** 16-byte fixed-size stack string for strategy attribution (`FixedString<16>`).
- **`OAuthToken`:** Authorization header (`Bearer <access_token>`) is pre-computed and pre-formatted at token acquisition/refresh time. Hot-path requests stream this pre-baked string with zero runtime allocations.

### 2. 64-Byte L1 CPU Cache Line Alignment
The core [`Order`](../include/schwab/order.hpp) structure is cache-line aligned:
```cpp
struct alignas(64) Order {
    Symbol symbol{};                  // 16 bytes
    StrategyTag tag{};                // 17 bytes
    double quantity{0.0};             //  8 bytes
    double price{0.0};                //  8 bytes
    OrderSide side{OrderSide::Buy};   //  1 byte
    OrderType type{OrderType::Market};//  1 byte
    OrderStatus status{OrderStatus::New};// 1 byte
    TimeInForce tif{TimeInForce::Day};//  1 byte
    // Total size padded exactly to 64 bytes (1 cache line)
};
static_assert(sizeof(Order) == 64, "Order struct must fit within a single 64-byte L1 cache line");
```
This guarantees that each order struct loads into CPU L1 cache with a single memory transaction, eliminating false sharing and cache line split penalties.

### 3. Native WinHTTP TLS Connection Pooling
Establishing a fresh TLS 1.3 socket over the internet typically incurs 50–200ms of handshake latency. The engine's [`HttpClient`](../include/schwab/http_client.hpp) leverages the Windows native WinHTTP connection pool:
- Pre-warms TLS sockets before order execution using lightweight authenticated probes (`/trader/v1/accounts/accountNumbers`).
- Reuses open keep-alive TCP/TLS sockets for subsequent orders, achieving sub-millisecond network dispatch.

---

## 2. End-to-End Execution Pipeline

```text
┌────────────────────────────────────────────────────────┐
│              schwab_executor execute                   │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ 1. OAuth Session Validation (TokenManager)             │
│    - Loads cached session; validates token TTL         │
│    - Pre-computed Bearer header ready on stack         │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ 2. Account & Portfolio Resolution (AccountManager)     │
│    - Queries live cash and holdings via WinHTTP        │
│    - Parses positions into contiguous std::vector      │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ 3. Strategy Order Generation (IStrategy)               │
│    - Evaluates portfolio drift against targets         │
│    - Emits raw orders with strategy attribution tags   │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ 4. Pre-Trade Risk Controls (RiskGuard)                 │
│    - Validates order notional <= max_order_notional    │
│    - Enforces share quantity & position exposure caps  │
│    - Checks price collar bounds (prevent fat-fingers)  │
│    - Enforces Market-On-Close (MOC) cutoff gates       │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ 5. Sell-First Sequencing & Dispatch (ExecutionDriver)  │
│    - Sequences liquidations and trims first            │
│    - Stages target acquisitions with unlocked cash     │
│    - Dispatches requests over pre-warmed WinHTTP pool  │
│    - Benchmarks per-order HTTP round-trip latency      │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼
┌────────────────────────────────────────────────────────┐
│ 6. Structured Telemetry & Audit Logs (ExecutionRunner) │
│    - Emits per-order latency and fill confirmations    │
│    - Appends structured ASCII reports to log file      │
└────────────────────────────────────────────────────────┘
```

---

## 3. Pre-Trade Risk Controls (`RiskGuard`)

Before any order is staged or transmitted, [`RiskGuard`](../include/schwab/risk_guard.hpp) evaluates each order through low-latency, zero-allocation pre-trade safety gates:

1. **Maximum Order Notional Bound:** Rejects orders where $\text{quantity} \times \text{price} > \text{max\_order\_notional}$ (configured via `SCHWAB_MAX_ORDER_NOTIONAL`).
2. **Maximum Order Quantity Bound:** Caps the maximum number of shares allowed in a single order (`max_order_quantity`).
3. **Position Exposure Cap:** Rejects orders that would push total post-trade exposure in a single symbol beyond `max_position_notional`.
4. **Price Collar Protection:** Rejects limit orders that deviate beyond configured percentage bounds (`price_collar_percent`, e.g. 5%) from the prevailing quote to prevent fat-finger executions.
5. **Market-On-Close (MOC) Cutoff Gate:** Rejects MOC orders submitted after the regulatory cutoff deadline (`moc_cutoff_seconds_utc`, e.g. 20:50 UTC / 15:50 EST) to prevent exchange rejections.

---

## 4. Full Codebase Directory Tree

```text
schwab-executor/
├── CMakeLists.txt                 # Modern C++20 build definition (MSVC / Clang / GCC)
├── build.bat                      # 1-click build script (Release / Debug / clean / test)
├── run_tests.bat                  # 1-click test runner & build script (Windows)
├── .env.example                   # Environment configuration template
├── .env                           # Local configuration (secrets, ignored by git)
├── .gitignore                     # Ignores build artifacts, secrets, tokens, and logs
├── docs/                          # Technical documentation
│   ├── ARCHITECTURE.md            # Low-latency architecture & memory design
│   ├── OPERATIONAL_GUIDE.md       # Production runbook & incident response
│   └── STRATEGY_DEVELOPMENT.md    # Quantitative model development guide
├── include/                       # Public C++ header files (.hpp)
│   └── schwab/
│       ├── account_manager.hpp    # Portfolio holdings & liquid cash query manager
│       ├── clock.hpp              # Monotonic precision clock & spin-wait utilities
│       ├── config.hpp             # Strongly-typed configuration parser & risk bounds
│       ├── execution_driver.hpp   # Sequential order dispatcher & execution rollback
│       ├── execution_runner.hpp   # Single-pass execution runner
│       ├── http_client.hpp        # WinHTTP client with socket pooling & TLS pre-warming
│       ├── http_types.hpp         # Zero-allocation HTTP request/response abstractions
│       ├── indicators.hpp         # Streaming Technical Indicators (SMA, EMA, RV)
│       ├── order.hpp              # 64-byte cache-aligned Order struct & domain types
│       ├── order_serializer.hpp   # Zero-allocation Schwab Trader API JSON serializer
│       ├── process_runner.hpp     # Subprocess execution & output capture
│       ├── rebalancer.hpp         # Account Portfolio drift analyzer & order sequencer
│       ├── risk_guard.hpp         # Multi-gate pre-trade risk controls
│       ├── strategy.hpp           # Abstract IStrategy interface & CompositeStrategy
│       ├── time.hpp               # Monotonic & wall-clock time helpers
│       ├── token.hpp              # OAuthToken model & pre-computed auth headers
│       └── token_manager.hpp      # Token lifecycle manager & JSON cache deserializer
├── src/                           # C++ implementation files (.cpp)
│   ├── account_manager.cpp
│   ├── clock.cpp
│   ├── config.cpp
│   ├── execution_driver.cpp
│   ├── execution_runner.cpp
│   ├── http_client.cpp
│   ├── http_types.cpp
│   ├── indicators.cpp
│   ├── main.cpp                   # Main CLI execution engine entry point
│   ├── order.cpp
│   ├── order_serializer.cpp
│   ├── process_runner.cpp
│   ├── rebalancer.cpp
│   ├── risk_guard.cpp
│   ├── strategy.cpp
│   ├── token.cpp
│   └── token_manager.cpp
├── tests/                         # Catch2 unit test suites (795 assertions)
│   ├── CMakeLists.txt
│   ├── test_account_manager.cpp
│   ├── test_clock.cpp
│   ├── test_config.cpp
│   ├── test_execution_driver.cpp
│   ├── test_execution_runner.cpp
│   ├── test_http_client.cpp
│   ├── test_http_types.cpp
│   ├── test_indicators.cpp
│   ├── test_order.cpp
│   ├── test_order_serializer.cpp
│   ├── test_process_runner.cpp
│   ├── test_rebalancer.cpp
│   ├── test_risk_guard.cpp
│   ├── test_sanity.cpp
│   ├── test_strategy.cpp
│   ├── test_time.cpp
│   ├── test_token.cpp
│   └── test_token_manager.cpp
└── python/                        # Python helper scripts
    ├── common.py                  # Shared constants & atomic token I/O
    ├── auth_cli.py                # Standalone OAuth 2.0 authentication manager
    └── status_cli.py              # Live account balance & portfolio diagnostic query
```

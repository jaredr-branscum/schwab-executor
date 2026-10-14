# Quantitative Strategy Development Guide

This guide details how to build, test, and integrate quantitative trading models with the `schwab-executor` engine.

---

## 1. Overview & Architectural Decoupling

The execution engine decouples quantitative decision logic from broker connectivity and order routing. All quantitative models implement the abstract [`IStrategy`](../include/schwab/strategy.hpp) interface:

```cpp
namespace schwab {

class IStrategy {
public:
    virtual ~IStrategy() = default;

    /// Strategy identifier for logging and telemetry attribution
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Evaluates portfolio state and generates target trading orders
    [[nodiscard]] virtual std::vector<Order> generate_orders(
        std::span<const Position> positions,
        double available_cash
    ) = 0;
};

} // namespace schwab
```

The engine provides three primary integration patterns:
- **Option A:** Configuration-Driven Target Rebalancing (Zero C++ code)
- **Option B:** In-Repository Custom Strategy Development
- **Option C:** External Private Repository Integration

---

## 2. Option A: Configuration-Driven Target Rebalancing (Zero C++ Code)

If your trading system executes target-weight portfolio rebalancing (e.g., fixed-weight asset allocations or model portfolios), no C++ code or recompilation is required.

### Step 1: Define Target Allocations in `.env`
Set the `SCHWAB_STRATEGY_TARGETS` key in your `.env` file using comma-separated `SYMBOL:WEIGHT` pairs:

```ini
# Target portfolio weights (must sum to <= 1.0)
SCHWAB_STRATEGY_TARGETS=SPY:0.60,QQQ:0.40
```

### Step 2: Validate in Simulation Mode
Run an immediate dry-run pass:

```powershell
.\build\Debug\schwab_executor.exe execute --dry-run
```

The engine queries live account equity and cash, calculates current holding weights, and generates the required orders:
- **Liquidations:** Any untracked holdings are sold first to release cash.
- **Trims:** Any overweight target positions are trimmed.
- **Acquisitions:** Underweight targets are purchased with available capital.

### Step 3: Execute Live
```powershell
.\build\Debug\schwab_executor.exe execute
```

---

## 3. Option B: In-Repository Custom Strategy Development

To build custom quantitative models (e.g., momentum, indicator-driven, mean-reversion) directly within this repository:

### Step 1: Create Your Strategy Header
Create a new header in `include/schwab/my_strategy.hpp`:

```cpp
#pragma once

#include "schwab/strategy.hpp"
#include "schwab/order.hpp"

class MyCustomStrategy : public schwab::IStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "MyCustomStrategy";
    }

    [[nodiscard]] std::vector<schwab::Order> generate_orders(
        std::span<const schwab::Position> positions,
        double available_cash
    ) override {
        std::vector<schwab::Order> orders;

        // Custom portfolio decision logic:
        // Example: Emit a market buy order for SPY
        schwab::Order order;
        order.symbol = schwab::Symbol("SPY");
        order.side = schwab::OrderSide::Buy;
        order.type = schwab::OrderType::Market;
        order.quantity = 10.0;
        order.tag = schwab::StrategyTag("LIVE_INTRADAY_SIGNAL");

        orders.push_back(order);
        return orders;
    }
};
```

### Step 2: Register the Strategy in `src/main.cpp`
Open [`src/main.cpp`](../src/main.cpp) and locate `resolve_strategy`:

```cpp
#include "schwab/my_strategy.hpp"

std::shared_ptr<schwab::IStrategy> resolve_strategy(const schwab::Config& cfg) {
    // Return your custom strategy instance:
    return std::make_shared<MyCustomStrategy>();
}
```

### Step 3: Rebuild and Run
```cmd
run_tests.bat
```

Test your strategy with dry-run telemetry:
```powershell
.\build\Debug\schwab_executor.exe execute --dry-run
```

---

## 4. Option C: External Private Repository Integration

To maintain strict separation between the open-source Schwab execution engine and proprietary quantitative strategies, models, and asset formulas:

### Step 1: Add as Submodule or Subdirectory
Add `schwab-executor` as a Git submodule or CMake dependency in your private repository:
```bash
git submodule add https://github.com/your-org/schwab-executor.git external/schwab-executor
```

### Step 2: Configure `CMakeLists.txt`
Link the `schwab_core` library target in your private `CMakeLists.txt`. Using `EXCLUDE_FROM_ALL` ensures that only the core engine library is compiled into your application (omitting executor root binaries and test suites):
```cmake
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/external/schwab-executor
    ${CMAKE_BINARY_DIR}/schwab-executor
    EXCLUDE_FROM_ALL
)

target_link_libraries(my_proprietary_trading_app PRIVATE
    schwab_core
)
```

### Step 3: Configure Environment & One-Time Authentication
1. Create a `.env` file in your application root directory with your Schwab Developer Portal credentials:
   ```ini
   SCHWAB_APP_KEY=your_app_key_here
   SCHWAB_APP_SECRET=your_app_secret_here
   SCHWAB_ACCOUNT_HASH=your_account_hash_here
   ```

2. Establish your initial OAuth session using the bundled Python authentication utility:
   ```bash
   python external/schwab-executor/python/auth_cli.py
   ```
   Follow the interactive prompt to log in and authorize the app. This creates `.schwab_token.json` in your project root. Ensure you add `.*token*.json` to your `.gitignore`.

> [!NOTE]
> **Automatic Token Lifecycle Management:**
> At runtime, `TokenManager` automatically:
> - Discovers `.schwab_token.json` from your project root, parent directory, or submodule path.
> - Automatically detects expired access tokens and silently invokes `auth_cli.py --refresh` in the background with zero user intervention.

### Step 4: Instantiate `ExecutionRunner`
```cpp
#include "schwab/execution_runner.hpp"
#include "my_proprietary_alpha_strategy.hpp"

int main() {
    schwab::ExecutionConfig config;
    config.session_name = "LIVE_PRODUCTION";

    auto strategy = std::make_shared<MyProprietaryAlphaStrategy>();
    schwab::ExecutionRunner runner(config, strategy);

    // Optional: Pre-warm TLS connection to eliminate cold-start execution jitter
    runner.http_client().prewarm();

    return runner.execute();
}
```

#### Advanced: Pre-Seeded Quotes for Sub-Millisecond Open Bell Dispatch
For time-critical strategies synchronized with market open (e.g. 09:30:00 AM ET), issuing synchronous REST API quote requests after the bell triggers adds 100–300ms of network round-trip delay.

Strategies can bypass the engine's internal quote fetch by setting `auto_fetch_quotes = false` and pre-seeding fresh quotes acquired immediately prior to market open (e.g. during premarket auction cross indications):

```cpp
schwab::ExecutionConfig config;
config.session_name = "MARKET_OPEN_DISPATCH";
// Bypass internal synchronous quote pre-fetching during execute()
config.auto_fetch_quotes = false;

schwab::ExecutionRunner runner(config, strategy);

// Pre-seed fresh quotes (e.g. acquired at T - 1.0s before the opening bell)
std::vector<schwab::Quote> pre_open_quotes = ...; 
runner.set_market_quotes(std::move(pre_open_quotes));

// Dispatches orders immediately upon invocation
return runner.execute();
```

This architecture ensures proprietary intellectual property remains 100% confidential in your private codebase while leveraging the shared, tested execution engine.

---

## 5. Multi-Strategy Composition (`CompositeStrategy`)

Multiple distinct models can be composed into a unified portfolio using [`CompositeStrategy`](../include/schwab/strategy.hpp). Orders emitted by each sub-model are automatically attributed with distinct strategy tags in execution telemetry:

```cpp
auto composite = std::make_shared<schwab::CompositeStrategy>("CompositePortfolio");
composite->add_strategy(std::make_shared<RebalanceModelA>());
composite->add_strategy(std::make_shared<MomentumModelB>());

schwab::ExecutionRunner runner(config, composite);
runner.execute();
```

---

## 6. Technical Indicators Library

The engine includes zero-heap, streaming technical indicators in [`include/schwab/indicators.hpp`](../include/schwab/indicators.hpp):

- **Simple Moving Average (`calculate_sma`):** Fast sliding window arithmetic mean.
- **Exponential Moving Average (`calculate_ema`):** Standard smoothing recursion ($k = 2 / (N + 1)$).
- **Realized Volatility (`calculate_realized_volatility`):** Annualized logarithmic return standard deviation with zero dynamic memory allocation (`std::array` fixed buffers).

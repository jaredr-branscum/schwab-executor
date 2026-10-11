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

1. Add `schwab-executor` as a Git submodule or CMake dependency in your private repository:
   ```powershell
   git submodule add https://github.com/your-org/schwab-executor.git external/schwab-executor
   ```

2. Link `schwab_core` in your private `CMakeLists.txt`:
   ```cmake
   add_subdirectory(external/schwab-executor)
   target_link_libraries(my_proprietary_trading_app PRIVATE schwab_core)
   ```

3. Instantiate `ExecutionRunner` in your private entry point:
   ```cpp
   #include "schwab/execution_runner.hpp"
   #include "my_proprietary_alpha_strategy.hpp"

   int main() {
       schwab::ExecutionConfig config;
       config.session_name = "LIVE_PRODUCTION";

       auto strategy = std::make_shared<MyProprietaryAlphaStrategy>();
       schwab::ExecutionRunner runner(config, strategy);

       return runner.execute();
   }
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

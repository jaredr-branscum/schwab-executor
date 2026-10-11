#pragma once

#include "schwab/order.hpp"

#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace schwab {

/**
 * @brief Abstract strategy interface for quantitative execution models.
 *
 * Decouples quantitative decision-making from execution engine infrastructure.
 */
class IStrategy {
public:
    virtual ~IStrategy() = default;

    /**
     * @brief Strategy identifier for logging and telemetry.
     */
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Evaluates portfolio state and generates target trading orders.
     *
     * @param positions Current portfolio positions held in the account.
     * @param available_cash Current available cash / buying power.
     * @return Sequenced vector of Order structs to stage and execute.
     */
    [[nodiscard]] virtual std::vector<Order> generate_orders(
        std::span<const Position> positions,
        double available_cash
    ) = 0;
};

/**
 * @brief Target allocation specification for generic rebalance strategy.
 */
struct StrategyTargetWeight {
    Symbol symbol{};
    double target_weight{0.0}; // [0.0 .. 1.0]

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return symbol.is_valid() && std::isfinite(target_weight) && target_weight >= 0.0 && target_weight <= 1.0;
    }
};

/**
 * @brief Generic reference implementation of IStrategy for public test suites and documentation.
 *
 * Implements a generic portfolio target-weight rebalancer.
 */
class SampleRebalanceStrategy : public IStrategy {
public:
    explicit SampleRebalanceStrategy(
        std::vector<StrategyTargetWeight> targets = {},
        OrderType order_type = OrderType::Market
    ) noexcept;

    [[nodiscard]] std::string_view name() const noexcept override {
        return "SampleRebalanceStrategy";
    }

    [[nodiscard]] std::vector<Order> generate_orders(
        std::span<const Position> positions,
        double available_cash
    ) override;

    [[nodiscard]] const std::vector<StrategyTargetWeight>& targets() const noexcept {
        return targets_;
    }

    [[nodiscard]] OrderType order_type() const noexcept {
        return order_type_;
    }

private:
    std::vector<StrategyTargetWeight> targets_;
    OrderType order_type_{OrderType::Market};
};

/**
 * @brief Multi-strategy composite orchestrator.
 *
 * Aggregates multiple sub-strategies and sequences their generated orders.
 * Automatically attributes orders with sub-strategy tags.
 */
class CompositeStrategy : public IStrategy {
public:
    explicit CompositeStrategy(std::string name = "CompositeStrategy") noexcept
        : name_(std::move(name)) {}

    void add_strategy(std::shared_ptr<IStrategy> strategy) {
        if (strategy) {
            strategies_.push_back(std::move(strategy));
        }
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }

    [[nodiscard]] std::vector<Order> generate_orders(
        std::span<const Position> positions,
        double available_cash
    ) override;

    [[nodiscard]] const std::vector<std::shared_ptr<IStrategy>>& strategies() const noexcept {
        return strategies_;
    }

private:
    std::string name_;
    std::vector<std::shared_ptr<IStrategy>> strategies_;
};

} // namespace schwab

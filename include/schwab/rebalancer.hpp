#pragma once

#include "schwab/market_data.hpp"
#include "schwab/order.hpp"
#include "schwab/risk_guard.hpp"

#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace schwab {

/**
 * @brief Target allocation for a single equity asset in a portfolio.
 */
struct TargetAllocation {
    Symbol symbol{};
    double target_shares{0.0};
    double current_price{0.0};
    std::optional<double> limit_price{std::nullopt};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        if (!symbol.is_valid() || !std::isfinite(target_shares) || target_shares < 0.0) {
            return false;
        }
        if (!std::isfinite(current_price) || current_price < 0.0) {
            return false;
        }
        if (limit_price.has_value() && (!std::isfinite(*limit_price) || *limit_price <= 0.0)) {
            return false;
        }
        return true;
    }
};

/**
 * @brief Configuration parameters for portfolio rebalance order generation.
 */
struct RebalancerOptions {
    OrderType order_type = OrderType::Market;
    TimeInForce duration = TimeInForce::Day;
    OrderSession session = OrderSession::Normal;
    double min_share_threshold = 0.0001;      // Ignore deltas smaller than this
    bool liquidate_untracked = true;          // Liquidate positions not present in target allocations
    bool abort_buys_on_sell_rejection = true; // Atomic abort: cancel all buys if any sell is blocked/rejected
};

/**
 * @brief Sequenced execution plan containing generated broker orders.
 */
struct RebalancePlan {
    std::vector<Order> orders;                  // Approved orders sequenced for execution (Sells first, Buys second)
    std::vector<Order> rejected_orders;         // Orders rejected by RiskGuard
    std::vector<std::string> rejection_reasons; // Rejection diagnostic messages
    bool all_approved{true};
    double total_sell_notional{0.0};
    double total_buy_notional{0.0};
};

/**
 * @brief Portfolio rebalancing engine
 *
 * Computes share deltas between target allocations and existing holdings,
 * enforces sells-first execution sequencing to free buying power,
 * and gates all orders through pre-trade risk controls.
 */
class PortfolioRebalancer {
public:
    /**
     * @brief Generates execution plan from target allocations and current positions.
     *
     * Sell orders are automatically sequenced before Buy orders to release capital and buying power.
     * Every order is evaluated against the optional RiskGuard before staging into plan.
     *
     * @param targets Target allocations for portfolio.
     * @param current_positions Current account positions.
     * @param risk_guard Optional pre-trade risk gate.
     * @param options Execution configuration options.
     * @return Generated RebalancePlan.
     */
    [[nodiscard]] static RebalancePlan generate_plan(
        std::span<const TargetAllocation> targets,
        std::span<const Position> current_positions,
        const RiskGuard* risk_guard = nullptr,
        const RebalancerOptions& options = {}
    );

    /**
     * @brief Generates execution plan from target allocations, current positions, and live quotes.
     *
     * Enforces following sequencing:
     * 1. Untracked Liquidations (100% full exits to maximize released capital)
     * 2. Target Trims (Partial position reductions)
     * 3. Target Buys (Staged after cash is unlocked)
     *
     * Orders are evaluated against real-time Quote data (spread circuit breakers,
     * crossed-book gates, and spread-aware collars) if quotes are provided.
     *
     * @param targets Target allocations for portfolio.
     * @param current_positions Current account positions.
     * @param quotes Real-time quotes for symbols being traded.
     * @param risk_guard Optional pre-trade risk gate.
     * @param options Execution configuration options.
     * @return Generated RebalancePlan.
     */
    [[nodiscard]] static RebalancePlan generate_plan(
        std::span<const TargetAllocation> targets,
        std::span<const Position> current_positions,
        std::span<const Quote> quotes,
        const RiskGuard* risk_guard = nullptr,
        const RebalancerOptions& options = {}
    );
};

} // namespace schwab

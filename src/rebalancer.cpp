#include "schwab/rebalancer.hpp"

#include <cmath>
#include <utility>

namespace schwab {

RebalancePlan PortfolioRebalancer::generate_plan(
    std::span<const TargetAllocation> targets,
    std::span<const Position> current_positions,
    const RiskGuard* risk_guard,
    const RebalancerOptions& options
) {
    return generate_plan(targets, current_positions, {}, risk_guard, options);
}

RebalancePlan PortfolioRebalancer::generate_plan(
    std::span<const TargetAllocation> targets,
    std::span<const Position> current_positions,
    std::span<const Quote> quotes,
    const RiskGuard* risk_guard,
    const RebalancerOptions& options
) {
    RebalancePlan plan;

    // Sanitize min share threshold against negative or NaN inputs
    const double min_threshold = (std::isfinite(options.min_share_threshold) && options.min_share_threshold > 0.0)
        ? options.min_share_threshold
        : 0.0001;

    // Pre-reserve plan output vectors
    plan.orders.reserve(targets.size() + current_positions.size());

    // Find quote for symbol
    auto find_quote = [&](const Symbol& symbol) -> const Quote* {
        for (const auto& q : quotes) {
            if (q.symbol == symbol && q.is_valid()) {
                return &q;
            }
        }
        return nullptr;
    };

    // Helper lambda to evaluate risk and append order directly to plan.orders
    auto evaluate_and_stage = [&](const Order& order, double current_shares, double fallback_price) {
        const Quote* q = find_quote(order.symbol);
        double ref_price = 0.0;
        if (q != nullptr) {
            ref_price = (order.type == OrderType::Limit && order.limit_price.has_value() && *order.limit_price > 0.0)
                ? *order.limit_price
                : q->reference_price();
        } else {
            ref_price = (order.type == OrderType::Limit && order.limit_price.has_value() && *order.limit_price > 0.0)
                ? *order.limit_price
                : fallback_price;
        }

        if (risk_guard != nullptr) {
            RiskCheckResult check;
            if (q != nullptr) {
                check = risk_guard->validate_order(order, *q, current_shares);
            } else {
                check = risk_guard->validate_order(order, current_shares, ref_price);
            }

            if (!check.approved) {
                plan.rejected_orders.push_back(order);
                plan.rejection_reasons.push_back(std::string(check.message));
                plan.all_approved = false;
                return;
            }
        }

        if (order.side == OrderSide::Sell) {
            plan.total_sell_notional += order.estimated_notional(ref_price);
        } else {
            plan.total_buy_notional += order.estimated_notional(ref_price);
        }

        plan.orders.push_back(order);
    };

    // Find existing shares and reference price for a symbol
    auto find_position = [&](const Symbol& symbol) -> std::pair<double, double> {
        for (const auto& pos : current_positions) {
            if (pos.symbol == symbol) {
                double price = (pos.current_price > 0.0) ? pos.current_price : pos.average_price;
                return {pos.quantity, price};
            }
        }
        return {0.0, 0.0};
    };

    // =========================================================================
    // PASS 1A: UNTRACKED LIQUIDATIONS (100% full exits to maximize released capital)
    // =========================================================================
    if (options.liquidate_untracked) {
        for (const auto& pos : current_positions) {
            if (!std::isfinite(pos.quantity) || pos.quantity <= min_threshold) {
                continue;
            }

            bool is_tracked = false;
            for (const auto& target : targets) {
                if (target.symbol == pos.symbol) {
                    is_tracked = true;
                    break;
                }
            }

            if (!is_tracked) {
                Order order;
                order.symbol = pos.symbol;
                order.tag = StrategyTag("LIQUIDATE");
                order.side = OrderSide::Sell;
                order.type = options.order_type;
                order.duration = options.duration;
                order.session = options.session;
                order.quantity = pos.quantity;
                order.limit_price = std::nullopt;
                order.status = OrderStatus::New;

                double fallback_price = (pos.current_price > 0.0) ? pos.current_price : pos.average_price;
                evaluate_and_stage(order, pos.quantity, fallback_price);
            }
        }
    }

    // =========================================================================
    // PASS 1B: TARGET TRIMS (Partial position reductions on tracked holdings)
    // =========================================================================
    for (const auto& target : targets) {
        if (!target.is_valid()) [[unlikely]] {
            continue;
        }

        auto [current_shares, pos_price] = find_position(target.symbol);
        const double delta = target.target_shares - current_shares;

        if (delta < -min_threshold) {
            Order order;
            order.symbol = target.symbol;
            order.tag = StrategyTag("REBALANCE");
            order.side = OrderSide::Sell;
            order.type = options.order_type;
            order.duration = options.duration;
            order.session = options.session;
            order.quantity = std::abs(delta);
            order.limit_price = target.limit_price;
            order.status = OrderStatus::New;

            double fallback_price = (target.current_price > 0.0) ? target.current_price : pos_price;
            evaluate_and_stage(order, current_shares, fallback_price);
        }
    }

    // Check if any sell order was rejected during Pass 1
    bool has_rejected_sells = false;
    for (const auto& rej : plan.rejected_orders) {
        if (rej.side == OrderSide::Sell) {
            has_rejected_sells = true;
            break;
        }
    }

    // =========================================================================
    // PASS 2: TARGET BUYS (Sized and Sequenced After Capital Has Been Unlocked)
    // =========================================================================
    for (const auto& target : targets) {
        if (!target.is_valid()) [[unlikely]] {
            continue;
        }

        auto [current_shares, pos_price] = find_position(target.symbol);
        const double delta = target.target_shares - current_shares;

        if (delta > min_threshold) {
            Order order;
            order.symbol = target.symbol;
            order.tag = StrategyTag("REBALANCE");
            order.side = OrderSide::Buy;
            order.type = options.order_type;
            order.duration = options.duration;
            order.session = options.session;
            order.quantity = delta;
            order.limit_price = target.limit_price;
            order.status = OrderStatus::New;

            if (options.abort_buys_on_sell_rejection && has_rejected_sells) [[unlikely]] {
                plan.rejected_orders.push_back(order);
                plan.rejection_reasons.push_back(
                    "Buy order aborted: preceding sell order was rejected (atomic abort)"
                );
                plan.all_approved = false;
                continue;
            }

            double fallback_price = (target.current_price > 0.0) ? target.current_price : pos_price;
            evaluate_and_stage(order, current_shares, fallback_price);
        }
    }

    return plan;
}

} // namespace schwab

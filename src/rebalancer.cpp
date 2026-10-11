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
    RebalancePlan plan;

    // Sanitize min share threshold against negative or NaN inputs
    const double min_threshold = (std::isfinite(options.min_share_threshold) && options.min_share_threshold > 0.0)
        ? options.min_share_threshold
        : 0.0001;

    // Pre-reserve plan output vectors
    plan.orders.reserve(targets.size() + current_positions.size());

    // Helper lambda to evaluate risk and append order directly to plan.orders
    auto evaluate_and_stage = [&](const Order& order, double current_shares, double ref_price) {
        if (risk_guard != nullptr) {
            auto check = risk_guard->validate_order(order, current_shares, ref_price);
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
    // PASS 1: SELLS (Liquidations & Position Reductions to Free Buying Power)
    // =========================================================================

    // Target reductions (target_shares < current_shares)
    for (const auto& target : targets) {
        if (!target.is_valid()) [[unlikely]] {
            continue;
        }

        auto [current_shares, pos_price] = find_position(target.symbol);
        const double delta = target.target_shares - current_shares;

        if (delta < -min_threshold) {
            Order order;
            order.symbol = target.symbol;
            order.side = OrderSide::Sell;
            order.type = options.order_type;
            order.duration = options.duration;
            order.session = options.session;
            order.quantity = std::abs(delta);
            order.limit_price = target.limit_price;
            order.status = OrderStatus::New;

            double ref_price = (target.current_price > 0.0) ? target.current_price : pos_price;
            evaluate_and_stage(order, current_shares, ref_price);
        }
    }

    // Untracked position liquidations (positions not in targets). Positions are liquidated entirely to free up capital.
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
                order.side = OrderSide::Sell;
                order.type = options.order_type;
                order.duration = options.duration;
                order.session = options.session;
                order.quantity = pos.quantity;
                order.limit_price = std::nullopt;
                order.status = OrderStatus::New;

                double ref_price = (pos.current_price > 0.0) ? pos.current_price : pos.average_price;
                evaluate_and_stage(order, pos.quantity, ref_price);
            }
        }
    }

    // =========================================================================
    // PASS 2: BUYS (Sized and Sequenced After Capital Has Been Unlocked)
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
            order.side = OrderSide::Buy;
            order.type = options.order_type;
            order.duration = options.duration;
            order.session = options.session;
            order.quantity = delta;
            order.limit_price = target.limit_price;
            order.status = OrderStatus::New;

            double ref_price = (target.current_price > 0.0) ? target.current_price : pos_price;
            evaluate_and_stage(order, current_shares, ref_price);
        }
    }

    return plan;
}

} // namespace schwab

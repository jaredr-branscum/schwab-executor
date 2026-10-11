#include "schwab/strategy.hpp"

#include <algorithm>
#include <cmath>

namespace schwab {

SampleRebalanceStrategy::SampleRebalanceStrategy(
    std::vector<StrategyTargetWeight> targets,
    OrderType order_type
) noexcept
    : targets_(std::move(targets)), order_type_(order_type) {}

std::vector<Order> SampleRebalanceStrategy::generate_orders(
    std::span<const Position> positions,
    double available_cash
) {
    if (targets_.empty()) {
        return {};
    }

    const OrderType ord_type = order_type_;

    // Calculate Total Portfolio Market Value
    double total_value = (available_cash > 0.0) ? available_cash : 0.0;
    for (const auto& pos : positions) {
        if (pos.quantity > 0.0 && pos.current_price > 0.0) {
            total_value += pos.quantity * pos.current_price;
        }
    }

    if (total_value <= 0.0) {
        return {};
    }

    std::vector<Order> orders;
    orders.reserve(positions.size() + targets_.size());

    // Generate SELL orders (untracked position liquidations & weight reductions)

    // Find untracked positions
    for (const auto& pos : positions) {
        if (pos.quantity <= 0.0001) {
            continue;
        }
        bool tracked = false;
        for (const auto& tgt : targets_) {
            if (pos.symbol == tgt.symbol) {
                tracked = true;
                break;
            }
        }
        if (!tracked) {
            Order sell_ord;
            sell_ord.symbol = pos.symbol;
            sell_ord.side = OrderSide::Sell;
            sell_ord.type = ord_type;
            sell_ord.quantity = pos.quantity;
            sell_ord.status = OrderStatus::New;
            sell_ord.tag = StrategyTag("LIQUIDATE");
            orders.push_back(sell_ord);
        }
    }

    // Target rebalance sell orders
    for (const auto& tgt : targets_) {
        if (!tgt.is_valid()) {
            continue;
        }

        double cur_qty = 0.0;
        double cur_price = 0.0;
        for (const auto& pos : positions) {
            if (pos.symbol == tgt.symbol) {
                cur_qty = pos.quantity;
                cur_price = pos.current_price;
                break;
            }
        }

        if (cur_price <= 0.0) {
            continue;
        }

        double desired_dollars = total_value * tgt.target_weight;
        double desired_shares = desired_dollars / cur_price;
        double delta = desired_shares - cur_qty;

        if (delta < -0.0001) {
            Order sell_ord;
            sell_ord.symbol = tgt.symbol;
            sell_ord.side = OrderSide::Sell;
            sell_ord.type = ord_type;
            sell_ord.quantity = std::abs(delta);
            sell_ord.status = OrderStatus::New;
            sell_ord.tag = StrategyTag("REBALANCE");
            orders.push_back(sell_ord);
        }
    }

    // Generate BUY orders (acquisitions & target portfolio weight increases)
    for (const auto& tgt : targets_) {
        if (!tgt.is_valid()) {
            continue;
        }

        double cur_qty = 0.0;
        double cur_price = 0.0;
        for (const auto& pos : positions) {
            if (pos.symbol == tgt.symbol) {
                cur_qty = pos.quantity;
                cur_price = pos.current_price;
                break;
            }
        }

        if (cur_price <= 0.0) {
            continue;
        }

        double desired_dollars = total_value * tgt.target_weight;
        double desired_shares = desired_dollars / cur_price;
        double delta = desired_shares - cur_qty;

        if (delta > 0.0001) {
            Order buy_ord;
            buy_ord.symbol = tgt.symbol;
            buy_ord.side = OrderSide::Buy;
            buy_ord.type = ord_type;
            buy_ord.quantity = delta;
            buy_ord.status = OrderStatus::New;
            buy_ord.tag = StrategyTag("REBALANCE");
            orders.push_back(buy_ord);
        }
    }

    return orders;
}

std::vector<Order> CompositeStrategy::generate_orders(
    std::span<const Position> positions,
    double available_cash
) {
    std::vector<Order> combined;
    for (const auto& strat : strategies_) {
        if (!strat) {
            continue;
        }
        auto sub_orders = strat->generate_orders(positions, available_cash);
        for (auto& ord : sub_orders) {
            if (ord.tag.empty()) {
                ord.tag = StrategyTag(strat->name());
            }
        }
        combined.insert(combined.end(),
            std::make_move_iterator(sub_orders.begin()),
            std::make_move_iterator(sub_orders.end()));
    }
    return combined;
}

} // namespace schwab

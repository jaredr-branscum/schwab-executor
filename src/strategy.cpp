#include "schwab/strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <charconv>

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

std::vector<StrategyTargetWeight> SampleRebalanceStrategy::parse_targets(std::string_view targets_str) {
    std::vector<StrategyTargetWeight> targets;
    size_t pos = 0;
    while (pos < targets_str.size()) {
        size_t next_comma = targets_str.find(',', pos);
        std::string_view token = (next_comma == std::string_view::npos)
            ? targets_str.substr(pos)
            : targets_str.substr(pos, next_comma - pos);
        pos = (next_comma == std::string_view::npos) ? targets_str.size() : next_comma + 1;

        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
            token.remove_prefix(1);
        }
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
            token.remove_suffix(1);
        }
        if (token.empty()) {
            continue;
        }

        size_t sep_pos = token.find_first_of(":=");
        if (sep_pos == std::string_view::npos || sep_pos == 0 || sep_pos == token.size() - 1) {
            continue;
        }

        std::string_view sym_sv = token.substr(0, sep_pos);
        std::string_view weight_sv = token.substr(sep_pos + 1);

        while (!sym_sv.empty() && std::isspace(static_cast<unsigned char>(sym_sv.front()))) {
            sym_sv.remove_prefix(1);
        }
        while (!sym_sv.empty() && std::isspace(static_cast<unsigned char>(sym_sv.back()))) {
            sym_sv.remove_suffix(1);
        }

        while (!weight_sv.empty() && std::isspace(static_cast<unsigned char>(weight_sv.front()))) {
            weight_sv.remove_prefix(1);
        }
        while (!weight_sv.empty() && std::isspace(static_cast<unsigned char>(weight_sv.back()))) {
            weight_sv.remove_suffix(1);
        }

        double weight = 0.0;
        auto [ptr, ec] = std::from_chars(weight_sv.data(), weight_sv.data() + weight_sv.size(), weight);
        if (ec == std::errc{} && ptr == weight_sv.data() + weight_sv.size()) {
            StrategyTargetWeight tw{Symbol(sym_sv), weight};
            if (tw.is_valid()) {
                targets.push_back(tw);
            }
        }
    }
    return targets;
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

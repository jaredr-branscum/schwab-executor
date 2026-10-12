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

std::vector<Symbol> SampleRebalanceStrategy::target_symbols() const {
    std::vector<Symbol> syms;
    syms.reserve(targets_.size());
    for (const auto& tgt : targets_) {
        if (tgt.is_valid()) {
            syms.push_back(tgt.symbol);
        }
    }
    return syms;
}

std::vector<Order> SampleRebalanceStrategy::generate_orders(
    std::span<const Position> positions,
    double available_cash,
    std::span<const Quote> quotes
) {
    if (targets_.empty()) {
        return {};
    }

    const OrderType ord_type = order_type_;

    // Pricing lookup prioritizing quotes then positions
    auto get_sell_price = [&](const Symbol& sym) noexcept -> double {
        for (const auto& q : quotes) {
            if (q.symbol == sym && q.is_valid()) {
                double p = (q.bid_price > 0.0) ? q.bid_price : q.reference_price();
                if (p > 0.0) return p;
            }
        }
        for (const auto& pos : positions) {
            if (pos.symbol == sym && pos.current_price > 0.0) {
                return pos.current_price;
            }
        }
        return 0.0;
    };

    auto get_buy_price = [&](const Symbol& sym) noexcept -> double {
        for (const auto& q : quotes) {
            if (q.symbol == sym && q.is_valid()) {
                double p = (q.ask_price > 0.0) ? q.ask_price : q.reference_price();
                if (p > 0.0) return p;
            }
        }
        for (const auto& pos : positions) {
            if (pos.symbol == sym && pos.current_price > 0.0) {
                return pos.current_price;
            }
        }
        return 0.0;
    };

    auto get_price = [&](const Symbol& sym) noexcept -> double {
        for (const auto& q : quotes) {
            if (q.symbol == sym && q.is_valid()) {
                double ref = q.reference_price();
                if (ref > 0.0) {
                    return ref;
                }
            }
        }
        for (const auto& pos : positions) {
            if (pos.symbol == sym && pos.current_price > 0.0) {
                return pos.current_price;
            }
        }
        return 0.0;
    };

    // Calculate Total Portfolio Market Value
    double total_value = (available_cash > 0.0) ? available_cash : 0.0;
    for (const auto& pos : positions) {
        if (pos.quantity > 0.0) {
            double p = get_price(pos.symbol);
            if (p <= 0.0) {
                p = (pos.current_price > 0.0) ? pos.current_price : 0.0;
            }
            if (p > 0.0) {
                total_value += pos.quantity * p;
            }
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
        for (const auto& pos : positions) {
            if (pos.symbol == tgt.symbol) {
                cur_qty = pos.quantity;
                break;
            }
        }

        double cur_price = get_price(tgt.symbol);
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
            sell_ord.quantity = std::floor((std::abs(delta) + 1e-6) * 100.0) / 100.0;
            sell_ord.status = OrderStatus::New;
            sell_ord.tag = StrategyTag("REBALANCE");
            orders.push_back(sell_ord);
        }
    }

    // Calculate total available purchasing power for buys: available cash + estimated sell proceeds
    double estimated_sell_proceeds = 0.0;
    for (const auto& ord : orders) {
        if (ord.side == OrderSide::Sell) {
            double sp = get_sell_price(ord.symbol);
            if (sp > 0.0) {
                estimated_sell_proceeds += ord.quantity * sp;
            }
        }
    }
    double total_cash_budget = (available_cash > 0.0 ? available_cash : 0.0) + estimated_sell_proceeds;

    // Stage BUY orders and measure total commitment
    struct StagedBuy {
        Symbol symbol;
        double delta_shares{0.0};
        double buy_price{0.0};
    };
    std::vector<StagedBuy> staged_buys;
    double total_buy_dollars = 0.0;

    for (const auto& tgt : targets_) {
        if (!tgt.is_valid()) {
            continue;
        }

        double cur_qty = 0.0;
        for (const auto& pos : positions) {
            if (pos.symbol == tgt.symbol) {
                cur_qty = pos.quantity;
                break;
            }
        }

        double buy_p = get_buy_price(tgt.symbol);
        if (buy_p <= 0.0) {
            continue;
        }

        double desired_dollars = total_value * tgt.target_weight;
        double desired_shares = desired_dollars / buy_p;
        double delta = desired_shares - cur_qty;

        if (delta > 0.0001) {
            staged_buys.push_back(StagedBuy{tgt.symbol, delta, buy_p});
            total_buy_dollars += delta * buy_p;
        }
    }

    // Scale back buys if aggregate commitments exceed cash budget (strict cash conservation)
    double scale = 1.0;
    if (total_buy_dollars > (total_cash_budget + 0.01) && total_buy_dollars > 0.0) {
        scale = total_cash_budget / total_buy_dollars;
    }

    double running_commitment = 0.0;
    for (const auto& b : staged_buys) {
        double max_affordable_shares = (b.buy_price > 0.0 && (total_cash_budget + 0.01) > running_commitment)
            ? (total_cash_budget + 0.01 - running_commitment) / b.buy_price
            : 0.0;
        double scaled_shares = b.delta_shares * scale;
        double final_shares = std::floor(((std::min)(scaled_shares, max_affordable_shares) + 1e-6) * 100.0) / 100.0;

        while (final_shares > 0.0001 && (running_commitment + (final_shares * b.buy_price)) > (total_cash_budget + 0.01)) {
            final_shares -= 0.01;
            final_shares = std::floor((final_shares + 1e-6) * 100.0) / 100.0;
        }

        if (final_shares > 0.0001) {
            Order buy_ord;
            buy_ord.symbol = b.symbol;
            buy_ord.side = OrderSide::Buy;
            buy_ord.type = ord_type;
            buy_ord.quantity = final_shares;
            buy_ord.status = OrderStatus::New;
            buy_ord.tag = StrategyTag("REBALANCE");
            orders.push_back(buy_ord);
            running_commitment += final_shares * b.buy_price;
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

std::vector<Symbol> CompositeStrategy::target_symbols() const {
    std::vector<Symbol> syms;
    for (const auto& strat : strategies_) {
        if (!strat) continue;
        auto sub_syms = strat->target_symbols();
        syms.insert(syms.end(), sub_syms.begin(), sub_syms.end());
    }
    return syms;
}

std::vector<Order> CompositeStrategy::generate_orders(
    std::span<const Position> positions,
    double available_cash,
    std::span<const Quote> quotes
) {
    std::vector<Order> combined;
    for (const auto& strat : strategies_) {
        if (!strat) {
            continue;
        }
        auto sub_orders = strat->generate_orders(positions, available_cash, quotes);
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

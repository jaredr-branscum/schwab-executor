#include "schwab/order_detail.hpp"

#include <nlohmann/json.hpp>
#include <charconv>
#include <algorithm>

namespace schwab {

namespace {

double parse_json_double(const nlohmann::json& j, const char* key, double default_val = 0.0) {
    if (!j.contains(key) || j[key].is_null()) {
        return default_val;
    }
    const auto& v = j[key];
    if (v.is_number()) {
        return v.get<double>();
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        double result = default_val;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
        if (ec == std::errc()) {
            return result;
        }
    }
    return default_val;
}

std::string parse_json_string(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) {
        return {};
    }
    const auto& v = j[key];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number()) {
        return v.dump();
    }
    return {};
}

} // namespace

OrderExecutionState OrderDetailParser::map_status(
    std::string_view status_str,
    double filled_qty,
    double total_qty
) noexcept {
    if (status_str.empty()) {
        return OrderExecutionState::Unknown;
    }

    if (status_str == "FILLED") {
        return OrderExecutionState::Filled;
    }
    if (status_str == "CANCELED" || status_str == "CANCELLED" || status_str == "PENDING_CANCEL") {
        return OrderExecutionState::Cancelled;
    }
    if (status_str == "REJECTED") {
        return OrderExecutionState::Rejected;
    }
    if (status_str == "EXPIRED") {
        return OrderExecutionState::Expired;
    }

    if (status_str == "WORKING" || status_str == "ACCEPTED" || status_str == "NEW") {
        if (filled_qty > 0.0 && (total_qty <= 0.0 || filled_qty < total_qty)) {
            return OrderExecutionState::PartiallyFilled;
        }
        if (total_qty > 0.0 && filled_qty >= total_qty) {
            return OrderExecutionState::Filled;
        }
        return OrderExecutionState::Working;
    }

    if (status_str == "QUEUED" || status_str == "PENDING_ACTIVATION" ||
        status_str == "AWAITING_PARENT_ORDER" || status_str == "AWAITING_CONDITION" ||
        status_str == "AWAITING_MANUAL_REVIEW" || status_str == "AWAITING_UR_OUT") {
        if (filled_qty > 0.0) {
            return OrderExecutionState::PartiallyFilled;
        }
        return OrderExecutionState::Queued;
    }

    return OrderExecutionState::Unknown;
}

std::optional<OrderDetail> OrderDetailParser::parse_order_detail(std::string_view json) {
    if (json.empty()) {
        return std::nullopt;
    }

    auto root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return std::nullopt;
    }

    OrderDetail detail;

    // Order ID
    if (root.contains("orderId")) {
        const auto& id_node = root["orderId"];
        if (id_node.is_string()) {
            detail.order_id = id_node.get<std::string>();
        } else if (id_node.is_number()) {
            detail.order_id = id_node.dump();
        }
    }

    // Quantities
    detail.quantity = parse_json_double(root, "quantity", 0.0);
    detail.filled_quantity = parse_json_double(root, "filledQuantity", 0.0);
    detail.remaining_quantity = parse_json_double(root, "remainingQuantity", 0.0);

    // Status
    detail.raw_status = parse_json_string(root, "status");
    detail.state = map_status(detail.raw_status, detail.filled_quantity, detail.quantity);

    // Order Type
    std::string type_str = parse_json_string(root, "orderType");
    if (type_str == "LIMIT") {
        detail.type = OrderType::Limit;
    } else if (type_str == "MARKET_ON_CLOSE") {
        detail.type = OrderType::MarketOnClose;
    } else {
        detail.type = OrderType::Market;
    }

    // Timestamps
    detail.entered_time = parse_json_string(root, "enteredTime");
    detail.close_time = parse_json_string(root, "closeTime");

    // Order Legs (Symbol and Instruction/Side)
    if (root.contains("orderLegCollection") && root["orderLegCollection"].is_array() && !root["orderLegCollection"].empty()) {
        const auto& first_leg = root["orderLegCollection"][0];
        if (first_leg.is_object()) {
            // Symbol from instrument object
            if (first_leg.contains("instrument") && first_leg["instrument"].is_object()) {
                std::string sym = parse_json_string(first_leg["instrument"], "symbol");
                if (!sym.empty()) {
                    detail.symbol = Symbol(sym);
                }
            }

            // Instruction (BUY / SELL)
            std::string instr = parse_json_string(first_leg, "instruction");
            if (instr == "SELL" || instr == "SELL_SHORT") {
                detail.side = OrderSide::Sell;
            } else {
                detail.side = OrderSide::Buy;
            }
        }
    }

    // Execution Activities (Leg Fill Prices -> Volume-Weighted Average Price)
    double total_cost = 0.0;
    double total_exec_shares = 0.0;

    if (root.contains("orderActivityCollection") && root["orderActivityCollection"].is_array()) {
        for (const auto& act : root["orderActivityCollection"]) {
            if (act.contains("executionLegs") && act["executionLegs"].is_array()) {
                for (const auto& leg : act["executionLegs"]) {
                    double leg_qty = parse_json_double(leg, "quantity", 0.0);
                    double leg_price = parse_json_double(leg, "price", 0.0);
                    if (leg_qty > 0.0 && leg_price > 0.0) {
                        total_cost += leg_qty * leg_price;
                        total_exec_shares += leg_qty;
                    }
                }
            }
        }
    }

    if (total_exec_shares > 0.0) {
        detail.average_fill_price = total_cost / total_exec_shares;
    } else if (root.contains("price") && root["price"].is_number()) {
        detail.average_fill_price = root["price"].get<double>();
    }

    return detail;
}

} // namespace schwab

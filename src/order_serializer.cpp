#include "schwab/order_serializer.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>

namespace schwab {

namespace {

std::string_view trim_sv(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n' || sv.back() == '/')) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

using namespace std::string_view_literals;

bool OrderSerializer::serialize_order(const Order& order, std::string& out_buffer) {
    if (!order.is_valid()) [[unlikely]] {
        return false;
    }

    out_buffer.clear();
    if (out_buffer.capacity() < 512) {
        out_buffer.reserve(512);
    }

    out_buffer.append("{\"orderType\":\""sv);
    out_buffer.append(to_string(order.type));
    out_buffer.append("\",\"session\":\""sv);
    out_buffer.append(to_string(order.session));
    out_buffer.append("\",\"duration\":\""sv);
    out_buffer.append(to_string(order.duration));
    out_buffer.append("\",\"orderStrategyType\":\"SINGLE\""sv);

    // Limit price if applicable
    if (order.type == OrderType::Limit && order.limit_price.has_value()) {
        out_buffer.append(",\"price\":"sv);
        char price_buf[32];
        auto [p_ptr, p_ec] = std::to_chars(price_buf, price_buf + sizeof(price_buf), *order.limit_price);
        if (p_ec == std::errc()) [[likely]] {
            out_buffer.append(price_buf, static_cast<size_t>(p_ptr - price_buf));
        } else [[unlikely]] {
            return false;
        }
    }

    out_buffer.append(",\"orderLegCollection\":[{\"instruction\":\""sv);
    out_buffer.append(to_string(order.side));
    out_buffer.append("\",\"quantity\":"sv);

    // Quantity (integer if whole number within 2^53 safe int range, otherwise floating point up to 4 decimal places)
    char qty_buf[32];
    constexpr double kMaxExactInt = 9007199254740992.0; // 2^53 max exact int in IEEE 754 double
    if (order.quantity < kMaxExactInt && order.quantity == std::floor(order.quantity)) {
        auto [q_ptr, q_ec] = std::to_chars(qty_buf, qty_buf + sizeof(qty_buf), static_cast<int64_t>(order.quantity));
        if (q_ec == std::errc()) [[likely]] {
            out_buffer.append(qty_buf, static_cast<size_t>(q_ptr - qty_buf));
        } else [[unlikely]] {
            return false;
        }
    } else {
        // Schwab equity rule: Quantity must have at most 15 digits and 4 decimal places
        double rounded_qty = std::round(order.quantity * 10000.0) / 10000.0;
        auto [q_ptr, q_ec] = std::to_chars(qty_buf, qty_buf + sizeof(qty_buf), rounded_qty, std::chars_format::fixed, 4);
        if (q_ec == std::errc()) [[likely]] {
            // Trim trailing zeros after decimal point for clean wire JSON (e.g. 1.5000 -> 1.5)
            char* end_ptr = q_ptr;
            char* dot_ptr = std::find(qty_buf, end_ptr, '.');
            if (dot_ptr != end_ptr) {
                while (end_ptr > dot_ptr + 1 && *(end_ptr - 1) == '0') {
                    --end_ptr;
                }
                if (end_ptr == dot_ptr + 1) {
                    --end_ptr;
                }
            }
            out_buffer.append(qty_buf, static_cast<size_t>(end_ptr - qty_buf));
        } else [[unlikely]] {
            return false;
        }
    }

    out_buffer.append(",\"instrument\":{\"symbol\":\""sv);
    out_buffer.append(order.symbol.view());
    out_buffer.append("\",\"assetType\":\"EQUITY\"}}]}"sv);

    return true;
}

std::string OrderSerializer::serialize_order(const Order& order) {
    std::string result;
    if (serialize_order(order, result)) {
        return result;
    }
    return {};
}

std::optional<std::string> OrderSerializer::extract_order_id(const HttpResponse& response) {
    auto loc_hdr = response.find_header("Location");
    if (!loc_hdr.has_value()) [[unlikely]] {
        return std::nullopt;
    }

    std::string_view loc = *loc_hdr;

    // Strip query parameters and fragment identifiers per RFC 3986 before path inspection
    size_t query_pos = loc.find('?');
    if (query_pos != std::string_view::npos) {
        loc = loc.substr(0, query_pos);
    }
    size_t hash_pos = loc.find('#');
    if (hash_pos != std::string_view::npos) {
        loc = loc.substr(0, hash_pos);
    }

    loc = trim_sv(loc);
    if (loc.empty()) [[unlikely]] {
        return std::nullopt;
    }

    size_t slash = loc.rfind('/');
    std::string_view id_part = (slash == std::string_view::npos) ? loc : loc.substr(slash + 1);
    if (id_part.empty()) [[unlikely]] {
        return std::nullopt;
    }

    return std::string(id_part);
}

std::string OrderSerializer::parse_error_message(const HttpResponse& response) {
    if (response.body.empty()) {
        return std::string(response.status_text());
    }

    auto j = nlohmann::json::parse(response.body, nullptr, false);
    if (!j.is_discarded() && j.is_object()) {
        std::string err_details;
        if (j.contains("errors") && j["errors"].is_array()) {
            for (const auto& item : j["errors"]) {
                if (item.is_string()) {
                    if (!err_details.empty()) {
                        err_details.append(", ");
                    }
                    err_details.append(item.get<std::string>());
                }
            }
        }

        std::string primary_msg;
        if (j.contains("error_description") && j["error_description"].is_string()) {
            primary_msg = j["error_description"].get<std::string>();
            if (j.contains("error") && j["error"].is_string()) {
                primary_msg = j["error"].get<std::string>() + ": " + primary_msg;
            }
        } else if (j.contains("description") && j["description"].is_string()) {
            primary_msg = j["description"].get<std::string>();
            if (j.contains("error") && j["error"].is_string()) {
                primary_msg = j["error"].get<std::string>() + ": " + primary_msg;
            }
        } else if (j.contains("message") && j["message"].is_string()) {
            primary_msg = j["message"].get<std::string>();
        } else if (j.contains("error") && j["error"].is_string()) {
            primary_msg = j["error"].get<std::string>();
        }

        if (!primary_msg.empty() && !err_details.empty()) {
            return primary_msg + ": " + err_details;
        }
        if (!err_details.empty()) {
            return err_details;
        }
        if (!primary_msg.empty()) {
            return primary_msg;
        }
    }

    return response.body;
}

void OrderSerializer::prewarm() noexcept {
    try {
        Order dummy{};
        dummy.symbol = Symbol("SPY");
        dummy.side = OrderSide::Buy;
        dummy.type = OrderType::Limit;
        dummy.quantity = 100.50;
        dummy.limit_price = 280.25;

        std::string buf;
        buf.reserve(512);
        serialize_order(dummy, buf);

        // Pre-warm extract_order_id lookup routines
        HttpResponse dummy_resp;
        dummy_resp.headers.push_back(HttpHeader{"Location", "/trader/v1/accounts/mock/orders/12345"});
        (void)extract_order_id(dummy_resp);
    } catch (...) {
        // Deterministic recovery: swallow exceptions during pre-flight warmup
    }
}

} // namespace schwab

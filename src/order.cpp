#include "schwab/order.hpp"
#include <algorithm>
#include <cctype>

namespace schwab {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        auto ca = static_cast<unsigned char>(a[i]);
        auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

std::optional<OrderSide> order_side_from_string(std::string_view str) noexcept {
    if (iequals(str, "BUY")) {
        return OrderSide::Buy;
    }
    if (iequals(str, "SELL")) {
        return OrderSide::Sell;
    }
    return std::nullopt;
}

std::optional<OrderType> order_type_from_string(std::string_view str) noexcept {
    if (iequals(str, "MARKET")) {
        return OrderType::Market;
    }
    if (iequals(str, "LIMIT")) {
        return OrderType::Limit;
    }
    if (iequals(str, "MARKET_ON_CLOSE") || iequals(str, "MOC")) {
        return OrderType::MarketOnClose;
    }
    return std::nullopt;
}

std::optional<TimeInForce> time_in_force_from_string(std::string_view str) noexcept {
    if (iequals(str, "DAY")) {
        return TimeInForce::Day;
    }
    if (iequals(str, "GTC") || iequals(str, "GOOD_TILL_CANCEL")) {
        return TimeInForce::Gtc;
    }
    return std::nullopt;
}

} // namespace schwab

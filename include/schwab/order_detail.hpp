#pragma once

#include "schwab/order.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace schwab {

/**
 * @brief Execution state for Charles Schwab order.
 */
enum class OrderExecutionState : uint8_t {
    Unknown,
    Queued,              ///< QUEUED, PENDING_ACTIVATION, AWAITING_PARENT_ORDER
    Working,             ///< WORKING, ACCEPTED, NEW
    Filled,              ///< FILLED
    PartiallyFilled,     ///< WORKING or QUEUED but filled_quantity > 0
    Cancelled,           ///< CANCELED, PENDING_CANCEL
    Rejected,            ///< REJECTED
    Expired              ///< EXPIRED
};

/**
 * @brief Zero-allocation string representation for OrderExecutionState.
 */
[[nodiscard]] constexpr std::string_view to_string(OrderExecutionState state) noexcept {
    switch (state) {
        case OrderExecutionState::Unknown: return "UNKNOWN";
        case OrderExecutionState::Queued: return "QUEUED";
        case OrderExecutionState::Working: return "WORKING";
        case OrderExecutionState::Filled: return "FILLED";
        case OrderExecutionState::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderExecutionState::Cancelled: return "CANCELLED";
        case OrderExecutionState::Rejected: return "REJECTED";
        case OrderExecutionState::Expired: return "EXPIRED";
    }
    return "UNKNOWN";
}

/**
 * @brief Converts granular OrderExecutionState to core OrderStatus.
 */
[[nodiscard]] constexpr OrderStatus to_order_status(OrderExecutionState state) noexcept {
    switch (state) {
        case OrderExecutionState::Filled: return OrderStatus::Filled;
        case OrderExecutionState::Cancelled:
        case OrderExecutionState::Expired: return OrderStatus::Cancelled;
        case OrderExecutionState::Rejected: return OrderStatus::Rejected;
        case OrderExecutionState::Working:
        case OrderExecutionState::PartiallyFilled:
        case OrderExecutionState::Queued: return OrderStatus::PendingSubmit;
        case OrderExecutionState::Unknown: return OrderStatus::New;
    }
    return OrderStatus::New;
}

/**
 * @brief Execution slippage metrics comparing executed fill price against reference quote price.
 */
struct SlippageMetric {
    double quote_price{0.0};       ///< Pre-trade reference quote price at dispatch
    double fill_price{0.0};        ///< Verified executed fill price (VWAP)
    double price_diff{0.0};        ///< Raw price difference (fill_price - quote_price)
    double slippage_bps{0.0};      ///< Directional slippage in bps (positive = adverse, negative = favorable)
    double abs_slippage_bps{0.0};  ///< Absolute slippage in bps |fill_price - quote_price| / quote_price * 10,000
    bool has_slippage{false};      ///< True if both quote_price > 0 and fill_price > 0
};

/**
 * @brief Computes directional and absolute slippage in basis points.
 *
 * For Buy: fill_price > quote_price is adverse (+bps), fill_price < quote_price is favorable (-bps).
 * For Sell: fill_price < quote_price is adverse (+bps), fill_price > quote_price is favorable (-bps).
 *
 * @param side OrderSide (Buy or Sell).
 * @param quote_price Pre-trade reference quote price.
 * @param fill_price Executed VWAP fill price.
 * @return Calculated SlippageMetric.
 */
[[nodiscard]] constexpr SlippageMetric calculate_slippage(
    OrderSide side,
    double quote_price,
    double fill_price
) noexcept {
    SlippageMetric m;
    if (quote_price <= 0.0 || fill_price <= 0.0) {
        return m;
    }
    m.quote_price = quote_price;
    m.fill_price = fill_price;
    m.price_diff = fill_price - quote_price;

    const double raw_diff = (fill_price > quote_price) 
        ? (fill_price - quote_price) 
        : (quote_price - fill_price);
    m.abs_slippage_bps = (raw_diff / quote_price) * 10000.0;

    if (side == OrderSide::Buy) {
        m.slippage_bps = ((fill_price - quote_price) / quote_price) * 10000.0;
    } else {
        m.slippage_bps = ((quote_price - fill_price) / quote_price) * 10000.0;
    }
    m.has_slippage = true;
    return m;
}

/**
 * @brief Detailed execution record extracted from Schwab Order API.
 */
struct OrderDetail {
    std::string order_id{};
    Symbol symbol{};
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Market};
    OrderExecutionState state{OrderExecutionState::Unknown};
    std::string raw_status{};            ///< Raw status string returned by Charles Schwab API
    double quantity{0.0};                ///< Total ordered share quantity
    double filled_quantity{0.0};         ///< Cumulative executed filled shares
    double remaining_quantity{0.0};      ///< Remaining unfilled shares
    double average_fill_price{0.0};      ///< Volume-weighted average executed price (VWAP)
    std::string entered_time{};          ///< Order placement timestamp string
    std::string close_time{};            ///< Order closure/execution timestamp string

    /// @brief Checks whether order has reached a terminal lifecycle state.
    [[nodiscard]] constexpr bool is_terminal() const noexcept {
        return state == OrderExecutionState::Filled ||
               state == OrderExecutionState::Cancelled ||
               state == OrderExecutionState::Rejected ||
               state == OrderExecutionState::Expired;
    }

    /// @brief Checks if order has completed its target share execution.
    [[nodiscard]] constexpr bool is_filled() const noexcept {
        return state == OrderExecutionState::Filled ||
               (quantity > 0.0 && filled_quantity >= quantity);
    }

    /// @brief Checks if order is active and working in the market.
    [[nodiscard]] constexpr bool is_working() const noexcept {
        return state == OrderExecutionState::Working ||
               state == OrderExecutionState::Queued ||
               state == OrderExecutionState::PartiallyFilled;
    }
};

/**
 * @brief Parser for Charles Schwab order details API (/trader/v1/accounts/{accountHash}/orders/{orderId}).
 */
class OrderDetailParser {
public:
    /**
     * @brief Parses single order detail JSON payload returned by Schwab.
     * Extracts orderId, status, quantities, and calculates execution leg VWAP.
     *
     * @param json Raw JSON response string from Schwab.
     * @return Parsed OrderDetail if valid, std::nullopt if payload was empty or invalid.
     */
    [[nodiscard]] static std::optional<OrderDetail> parse_order_detail(std::string_view json);

    /**
     * @brief Maps Charles Schwab status string to normalized OrderExecutionState.
     */
    [[nodiscard]] static OrderExecutionState map_status(
        std::string_view status_str,
        double filled_qty,
        double total_qty
    ) noexcept;
};

} // namespace schwab

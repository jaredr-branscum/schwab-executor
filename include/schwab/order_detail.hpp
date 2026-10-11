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

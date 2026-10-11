#pragma once

#include "schwab/order.hpp"
#include <chrono>
#include <string_view>
#include <cstdint>
#include <cmath>

namespace schwab {

/// @brief Pre-trade rejection classification
enum class RiskRejectReason : uint8_t {
    None,
    InvalidOrderStructure,
    ExceedsMaxOrderNotional,
    ExceedsMaxOrderQuantity,
    ExceedsMaxPositionNotional,
    PriceCollarViolation,
    MocCutoffExceeded,
    DryRunIntercepted
};

// Zero-allocation string representation for reject reasons
[[nodiscard]] constexpr std::string_view to_string(RiskRejectReason reason) noexcept {
    switch (reason) {
        case RiskRejectReason::None: return "NONE";
        case RiskRejectReason::InvalidOrderStructure: return "INVALID_ORDER_STRUCTURE";
        case RiskRejectReason::ExceedsMaxOrderNotional: return "EXCEEDS_MAX_ORDER_NOTIONAL";
        case RiskRejectReason::ExceedsMaxOrderQuantity: return "EXCEEDS_MAX_ORDER_QUANTITY";
        case RiskRejectReason::ExceedsMaxPositionNotional: return "EXCEEDS_MAX_POSITION_NOTIONAL";
        case RiskRejectReason::PriceCollarViolation: return "PRICE_COLLAR_VIOLATION";
        case RiskRejectReason::MocCutoffExceeded: return "MOC_CUTOFF_EXCEEDED";
        case RiskRejectReason::DryRunIntercepted: return "DRY_RUN_INTERCEPTED";
    }
    return "UNKNOWN";
}

/// @brief Result of a pre-trade risk evaluation
struct RiskCheckResult {
    bool approved{false};
    RiskRejectReason reason{RiskRejectReason::None};
    std::string_view message{"Approved"};

    [[nodiscard]] constexpr operator bool() const noexcept {
        return approved;
    }
};
static_assert(sizeof(RiskCheckResult) <= 32, "RiskCheckResult must be compact and register-allocatable");

/// @brief Pre-trade risk parameters
/// Struct contents should fit within single 64-byte L1 CPU cache line
struct RiskLimits {
    double max_order_notional{100'000.0};       // Max $ per single order (offset 0..7)
    double max_order_quantity{10'000.0};        // Max share count per single order (offset 8..15)
    double max_position_notional{500'000.0};    // Max $ position exposure per symbol (offset 16..23)
    double price_collar_percent{0.05};          // Max allowed price deviation (5%) (offset 24..31)
    uint32_t moc_cutoff_seconds_utc{75'000};    // Cutoff in seconds since UTC midnight (75,000s = 20:50:00 UTC / 15:50 EST) (offset 32..35)
    bool enforce_moc_cutoff{true};              // Enforce exchange MOC cutoff deadline (offset 36)
    bool dry_run{false};                        // Intercept orders when dry-run is active (offset 37)
};
static_assert(sizeof(RiskLimits) <= 64, "RiskLimits must fit within a single 64-byte CPU cache line");

/// @brief High-speed, zero-allocation pre-trade risk guard
class RiskGuard {
public:
    constexpr explicit RiskGuard(const RiskLimits& limits = {}) noexcept : limits_(limits) {}

    /// @brief Evaluates an order against pre-trade risk thresholds (inlined on hot path)
    /// @param order The order to validate
    /// @param current_position_quantity Existing shares held in this symbol (default 0.0)
    /// @param market_price Latest reference market price (used for Market/MOC orders and collars)
    /// @param current_time_seconds_utc Current time in seconds since midnight UTC (0 for auto now())
    /// @return RiskCheckResult indicating approval or rejection reason
    [[nodiscard]] inline RiskCheckResult validate_order(
        const Order& order,
        double current_position_quantity = 0.0,
        double market_price = 0.0,
        uint32_t current_time_seconds_utc = 0) const noexcept {

        if (!order.is_valid()) [[unlikely]] {
            return RiskCheckResult{
                .approved = false,
                .reason = RiskRejectReason::InvalidOrderStructure,
                .message = "Order fails basic structural validity (empty symbol, non-positive quantity, or missing limit price)"
            };
        }

        // Market-On-Close (MOC) Regulatory Cutoff Gate
        if (order.type == OrderType::MarketOnClose && limits_.enforce_moc_cutoff) [[unlikely]] {
            uint32_t sec_utc = current_time_seconds_utc;
            if (sec_utc == 0) {
                auto now = std::chrono::system_clock::now();
                auto dp = std::chrono::floor<std::chrono::days>(now);
                sec_utc = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now - dp).count());
            }
            if (sec_utc > limits_.moc_cutoff_seconds_utc) [[unlikely]] {
                return RiskCheckResult{
                    .approved = false,
                    .reason = RiskRejectReason::MocCutoffExceeded,
                    .message = "Market-on-close order rejected: submitted after exchange MOC cutoff deadline (20:50 UTC / 15:50 EST)"
                };
            }
        }

        // Reference Price Resolution
        double ref_price = 0.0;
        if (order.type == OrderType::Limit && order.limit_price.has_value() && *order.limit_price > 0.0) {
            ref_price = *order.limit_price;
        } else if (market_price > 0.0) {
            ref_price = market_price;
        } else [[unlikely]] {
            return RiskCheckResult{
                .approved = false,
                .reason = RiskRejectReason::InvalidOrderStructure,
                .message = "Market/MOC order lacks positive reference market price for valuation"
            };
        }

        // Single Order Notional Limit Check ($ Value)
        const double notional = order.quantity * ref_price;
        if (limits_.max_order_notional > 0.0 && notional > limits_.max_order_notional) [[unlikely]] {
            return RiskCheckResult{
                .approved = false,
                .reason = RiskRejectReason::ExceedsMaxOrderNotional,
                .message = "Order notional exceeds max_order_notional threshold"
            };
        }

        // Single Order Quantity Limit Check (Shares)
        if (limits_.max_order_quantity > 0.0 && order.quantity > limits_.max_order_quantity) [[unlikely]] {
            return RiskCheckResult{
                .approved = false,
                .reason = RiskRejectReason::ExceedsMaxOrderQuantity,
                .message = "Order quantity exceeds max_order_quantity threshold"
            };
        }

        // Price Collar Check (Limit Orders vs Market Price)
        if (order.type == OrderType::Limit && market_price > 0.0 && limits_.price_collar_percent > 0.0) {
            const double limit_val = *order.limit_price;
            if (order.side == OrderSide::Buy) {
                const double max_allowed = market_price * (1.0 + limits_.price_collar_percent);
                if (limit_val > max_allowed) [[unlikely]] {
                    return RiskCheckResult{
                        .approved = false,
                        .reason = RiskRejectReason::PriceCollarViolation,
                        .message = "Buy limit price exceeds upper price collar"
                    };
                }
            } else if (order.side == OrderSide::Sell) {
                const double min_allowed = market_price * (1.0 - limits_.price_collar_percent);
                if (limit_val < min_allowed) [[unlikely]] {
                    return RiskCheckResult{
                        .approved = false,
                        .reason = RiskRejectReason::PriceCollarViolation,
                        .message = "Sell limit price breaches lower price collar"
                    };
                }
            }
        }

        // Resulting Position Notional Limit Check
        if (limits_.max_position_notional > 0.0) {
            const double delta = (order.side == OrderSide::Buy) ? order.quantity : -order.quantity;
            const double resulting_quantity = current_position_quantity + delta;
            const double resulting_notional = std::abs(resulting_quantity) * ref_price;

            if (resulting_notional > limits_.max_position_notional) [[unlikely]] {
                return RiskCheckResult{
                    .approved = false,
                    .reason = RiskRejectReason::ExceedsMaxPositionNotional,
                    .message = "Resulting position notional exceeds max_position_notional threshold"
                };
            }
        }

        // Dry-Run Interceptor
        if (limits_.dry_run) [[unlikely]] {
            return RiskCheckResult{
                .approved = false,
                .reason = RiskRejectReason::DryRunIntercepted,
                .message = "Order intercepted: dry-run mode active"
            };
        }

        // All Pre-Trade Risk Gates Passed
        return RiskCheckResult{
            .approved = true,
            .reason = RiskRejectReason::None,
            .message = "Approved"
        };
    }

    [[nodiscard]] constexpr const RiskLimits& limits() const noexcept {
        return limits_;
    }

    constexpr void set_limits(const RiskLimits& limits) noexcept {
        limits_ = limits;
    }

private:
    RiskLimits limits_{};
};

} // namespace schwab

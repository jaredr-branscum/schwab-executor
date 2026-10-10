#pragma once

#include <array>
#include <algorithm>
#include <string_view>
#include <optional>
#include <cstdint>

namespace schwab {

/// @brief Stack-allocated fixed-capacity string for ticker symbols (zero heap allocation)
/// Aligned to 16 bytes (128-bit SIMD register width) to enable single-instruction vector moves
template <size_t N = 16>
struct FixedString {
    std::array<char, N> data_{};

    constexpr FixedString() noexcept { data_.fill('\0'); }

    // Compile-time literal validation: fails build immediately if string literal is too long
    template <size_t M>
    constexpr FixedString(const char (&str)[M]) noexcept {
        static_assert(M <= N, "Symbol literal exceeds maximum buffer capacity of FixedString!");
        data_.fill('\0');
        for (size_t i = 0; i < M - 1; ++i) {
            data_[i] = str[i];
        }
    }

    constexpr FixedString(const char* str) noexcept
        : FixedString(str ? std::string_view(str) : std::string_view{}) {}

    // Runtime string_view validation: uses [[unlikely]] branch to reject oversized symbols without truncation
    constexpr FixedString(std::string_view sv) noexcept {
        data_.fill('\0');
        if (sv.size() >= N) [[unlikely]] {
            // Refuse silent truncation: leave empty (data_[0] = '\0') so is_valid() rejects it
            return;
        }
        for (size_t i = 0; i < sv.size(); ++i) {
            data_[i] = sv[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        size_t len = 0;
        while (len < N && data_[len] != '\0') {
            ++len;
        }
        return std::string_view{data_.data(), len};
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return view();
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return data_[0] == '\0';
    }

    [[nodiscard]] constexpr const char* c_str() const noexcept {
        return data_.data();
    }

    [[nodiscard]] constexpr size_t size() const noexcept {
        return view().size();
    }

    [[nodiscard]] constexpr bool operator==(std::string_view other) const noexcept {
        return view() == other;
    }
};

using Symbol = FixedString<16>;

/// @brief Trading order side (direction)
enum class OrderSide : uint8_t {
    Buy,
    Sell
};

/// @brief Execution order type
enum class OrderType : uint8_t {
    Market,
    Limit,
    MarketOnClose
};

/// @brief Order time in force duration
enum class TimeInForce : uint8_t {
    Day,
    Gtc
};

/// @brief Lifecycle state of an order
enum class OrderStatus : uint8_t {
    New,
    PendingSubmit,
    Filled,
    Cancelled,
    Rejected
};

// Zero-allocation string conversions
[[nodiscard]] constexpr std::string_view to_string(OrderSide side) noexcept {
    switch (side) {
        case OrderSide::Buy: return "BUY";
        case OrderSide::Sell: return "SELL";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::Market: return "MARKET";
        case OrderType::Limit: return "LIMIT";
        case OrderType::MarketOnClose: return "MARKET_ON_CLOSE";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::Day: return "DAY";
        case TimeInForce::Gtc: return "GTC";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view to_string(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::New: return "NEW";
        case OrderStatus::PendingSubmit: return "PENDING_SUBMIT";
        case OrderStatus::Filled: return "FILLED";
        case OrderStatus::Cancelled: return "CANCELLED";
        case OrderStatus::Rejected: return "REJECTED";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::optional<OrderSide> order_side_from_string(std::string_view str) noexcept;
[[nodiscard]] std::optional<OrderType> order_type_from_string(std::string_view str) noexcept;
[[nodiscard]] std::optional<TimeInForce> time_in_force_from_string(std::string_view str) noexcept;

/// @brief Execution order specification
/// Struct contents should fit within single 64-byte L1 CPU cache line
struct Order {
    double quantity{0.0};
    std::optional<double> limit_price{std::nullopt};

    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Market};
    TimeInForce duration{TimeInForce::Day};
    OrderStatus status{OrderStatus::New};

    // Stack symbol buffer (12 bytes, fits any US equity ticker with zero heap allocation)
    Symbol symbol{};

    /// @brief Validates structural sanity of the order (inlined on hot path)
    [[nodiscard]] inline bool is_valid() const noexcept {
        if (symbol.empty() || quantity <= 0.0) {
            return false;
        }
        if (type == OrderType::Limit) {
            return limit_price.has_value() && *limit_price > 0.0;
        }
        return true;
    }

    /// @brief Computes estimated dollar notional (inlined on hot path)
    /// @param fallback_price Price used for Market/MOC orders when limit_price is absent
    [[nodiscard]] inline double estimated_notional(double fallback_price = 0.0) const noexcept {
        if (quantity <= 0.0) {
            return 0.0;
        }
        if (type == OrderType::Limit && limit_price.has_value() && *limit_price > 0.0) {
            return quantity * (*limit_price);
        }
        if (fallback_price > 0.0) {
            return quantity * fallback_price;
        }
        return 0.0;
    }
};
static_assert(sizeof(Order) <= 64, "Order must fit within a single 64-byte CPU cache line");

/// @brief Account position snapshot
/// Struct contents should fit within single 64-byte L1 CPU cache line
struct Position {
    double quantity{0.0};
    double average_price{0.0};
    double current_price{0.0};

    Symbol symbol{};

    [[nodiscard]] inline double market_value() const noexcept {
        return quantity * current_price;
    }

    [[nodiscard]] inline double cost_basis() const noexcept {
        return quantity * average_price;
    }

    [[nodiscard]] inline double unrealized_pnl() const noexcept {
        return market_value() - cost_basis();
    }

    [[nodiscard]] inline double unrealized_pnl_percent() const noexcept {
        const double basis = cost_basis();
        return (basis > 0.0) ? (unrealized_pnl() / basis) : 0.0;
    }
};
static_assert(sizeof(Position) <= 64, "Position must fit within a single 64-byte CPU cache line");

} // namespace schwab

#pragma once

#include <span>
#include <optional>
#include <cstddef>

namespace schwab {

/// @brief Computes the Simple Moving Average (SMA) of the last `period` elements
/// @param prices Non-owning contiguous slice of historical prices (vector, array, or span)
/// @param period Lookback window size (must be > 0 and <= prices.size())
/// @return Arithmetic mean of the trailing `period` elements, or std::nullopt if insufficient data
[[nodiscard]] std::optional<double> sma(std::span<const double> prices, size_t period) noexcept;

/// @brief Computes the Exponential Moving Average (EMA) of `prices` over lookback `period`
/// @param prices Non-owning contiguous slice of historical prices (vector, array, or span)
/// @param period Lookback window size (must be > 0 and <= prices.size())
/// @return Exponential moving average seeded with the initial `period` SMA, or std::nullopt if insufficient data
[[nodiscard]] std::optional<double> ema(std::span<const double> prices, size_t period) noexcept;

} // namespace schwab

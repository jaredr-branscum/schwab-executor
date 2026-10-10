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

} // namespace schwab

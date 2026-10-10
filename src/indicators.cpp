#include "schwab/indicators.hpp"
#include <cmath>

namespace schwab {

std::optional<double> sma(std::span<const double> prices, size_t period) noexcept {
    if (period == 0 || prices.size() < period) {
        return std::nullopt;
    }

    // Zero-allocation trailing subspan slice
    std::span<const double> window = prices.subspan(prices.size() - period, period);

    double sum = 0.0;
    for (double price : window) {
        sum += price;
    }

    return sum / static_cast<double>(period);
}

std::optional<double> ema(std::span<const double> prices, size_t period) noexcept {
    if (period == 0 || prices.size() < period) {
        return std::nullopt;
    }

    // Seed EMA with the simple moving average of the first `period` elements
    auto seed = sma(prices.subspan(0, period), period);
    if (!seed.has_value()) {
        return std::nullopt;
    }

    const double alpha = 2.0 / static_cast<double>(period + 1);
    const double one_minus_alpha = 1.0 - alpha;
    double current_ema = *seed;

    // Stream through remaining prices updating EMA recursively in register memory
    // Uses hardware FMA (vfmadd) to collapse multiplication and addition into a single 4-cycle CPU instruction
    for (size_t i = period; i < prices.size(); ++i) {
        current_ema = std::fma(one_minus_alpha, current_ema, alpha * prices[i]);
    }

    return current_ema;
}

} // namespace schwab

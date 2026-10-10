#include "schwab/indicators.hpp"

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

} // namespace schwab

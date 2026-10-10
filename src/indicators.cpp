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

std::optional<double> realized_volatility(
    std::span<const double> prices,
    size_t period,
    double annualization_factor) noexcept {
    // Requires at least 2 return periods and period + 1 prices for sample standard deviation
    if (period < 2 || prices.size() < period + 1 || annualization_factor <= 0.0) {
        return std::nullopt;
    }

    // Trailing window of (period + 1) prices to generate `period` returns
    std::span<const double> window = prices.subspan(prices.size() - (period + 1), period + 1);

    // Verify all prices in the window are positive (log is undefined for <= 0)
    for (double price : window) {
        if (price <= 0.0) {
            return std::nullopt;
        }
    }

    // Mathematical optimization:
    // The sum of log returns is telescoping:
    // sum(ln(P_k / P_{k-1})) = ln(P_N) - ln(P_0) = ln(P_N / P_0)
    // Therefore, the exact mean log return can be calculated in O(1) time:
    const double mean_ret = std::log(window.back() / window.front()) / static_cast<double>(period);

    // Single linear pass accumulating squared deviations from the mean
    // Hardware FMA accumulates (diff * diff + sum_sq) in a single instruction with zero intermediate rounding
    double sum_sq_diff = 0.0;
    for (size_t i = 1; i < window.size(); ++i) {
        const double r = std::log(window[i] / window[i - 1]);
        const double diff = r - mean_ret;
        sum_sq_diff = std::fma(diff, diff, sum_sq_diff);
    }

    // Sample variance with (N - 1) degrees of freedom
    const double sample_variance = sum_sq_diff / static_cast<double>(period - 1);

    return std::sqrt(sample_variance * annualization_factor);
}

} // namespace schwab

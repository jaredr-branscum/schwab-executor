#pragma once

#include "schwab/order.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace schwab {

/**
 * @brief High-speed, 64-byte L1 CPU cache line aligned market quote.
 *
 * Encapsulates National Best Bid and Offer (NBBO), trade mark price, volume,
 * and timestamp extracted from Charles Schwab Market Data API (/marketdata/v1/quotes).
 */
struct Quote {
    Symbol symbol{};             ///< 16-byte contiguous symbol (FixedString<16>)
    double bid_price{0.0};       ///< National best bid price
    double ask_price{0.0};       ///< National best ask/offer price
    double last_price{0.0};      ///< Last traded execution price
    double mark_price{0.0};      ///< Schwab mark price (midpoint or last traded quote)
    uint64_t total_volume{0};    ///< Daily cumulative share volume
    uint64_t quote_time_ms{0};   ///< Quote timestamp in milliseconds since UTC epoch

    /**
     * @brief Checks if quote has valid structural data and positive price values.
     */
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        if (!symbol.is_valid()) {
            return false;
        }
        return (mark_price > 0.0 && mark_price < 1e15) ||
               (last_price > 0.0 && last_price < 1e15) ||
               (bid_price > 0.0 && ask_price > 0.0 && bid_price < 1e15 && ask_price < 1e15);
    }

    /**
     * @brief Detects if order book is crossed (bid > ask), typically caused by market surges,
     *        fast book updates, or quote latency discrepancies.
     */
    [[nodiscard]] constexpr bool is_crossed() const noexcept {
        return bid_price > 0.0 && ask_price > 0.0 && (bid_price > ask_price);
    }

    /**
     * @brief Detects if order book is locked (bid == ask).
     */
    [[nodiscard]] constexpr bool is_locked() const noexcept {
        if (bid_price <= 0.0 || ask_price <= 0.0) {
            return false;
        }
        const double diff = bid_price - ask_price;
        return (diff >= -1e-7 && diff <= 1e-7);
    }

    /**
     * @brief Resolves best reference valuation price.
     *
     * Prioritizes mark price -> last traded price -> mid-market midpoint.
     * When order book is crossed, mark price and last traded price are prioritized
     * to avoid distorted mid-market quotes.
     */
    [[nodiscard]] constexpr double reference_price() const noexcept {
        if (mark_price > 0.0 && mark_price < 1e15) {
            return mark_price;
        }
        if (last_price > 0.0 && last_price < 1e15) {
            return last_price;
        }
        if (bid_price > 0.0 && ask_price > 0.0 && bid_price < 1e15 && ask_price < 1e15) {
            return (bid_price + ask_price) * 0.5;
        }
        return 0.0;
    }

    /**
     * @brief Calculates bid-ask spread in dollars.
     *
     * Defends against crossed/inverted order books (where ask < bid during book surges).
     * If crossed, returns 0.0 to prevent negative spread distortions. Callers should
     * check is_crossed() to detect anomalous quotes.
     */
    [[nodiscard]] constexpr double spread() const noexcept {
        if (bid_price <= 0.0 || ask_price <= 0.0) {
            return 0.0;
        }
        return (ask_price > bid_price) ? (ask_price - bid_price) : 0.0;
    }

    /**
     * @brief Calculates raw signed spread without clamping (negative if crossed).
     */
    [[nodiscard]] constexpr double raw_spread() const noexcept {
        return ask_price - bid_price;
    }

    /**
     * @brief Calculates spread in basis points (bps) relative to reference price.
     *
     * Returns 0.0 if reference price is invalid or if book is crossed/locked.
     */
    [[nodiscard]] constexpr double spread_bps() const noexcept {
        const double ref = reference_price();
        if (ref <= 0.0) {
            return 0.0;
        }
        return (spread() / ref) * 10'000.0;
    }
};

static_assert(sizeof(Quote) == 64, "Quote struct must fit within single 64-byte L1 CPU cache line");

/**
 * @brief Zero-heap parser for Charles Schwab /marketdata/v1/quotes JSON responses.
 */
class QuoteParser {
public:
    /**
     * @brief Parses single Quote from Schwab Market Data JSON response.
     *
     * @param json_content Raw JSON payload string.
     * @param target_symbol Symbol to extract.
     * @return Quote if successfully parsed, nullopt otherwise.
     */
    [[nodiscard]] static std::optional<Quote> parse_single(
        std::string_view json_content,
        std::string_view target_symbol
    ) noexcept;

    /**
     * @brief Parses multiple Quotes from Schwab batch /marketdata/v1/quotes JSON response.
     *
     * @param json_content Raw JSON payload string.
     * @return Vector of valid parsed quotes.
     */
    [[nodiscard]] static std::vector<Quote> parse_batch(
        std::string_view json_content
    ) noexcept;
};

} // namespace schwab

#pragma once
 
#include "schwab/http_client.hpp"
#include "schwab/order.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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

/**
 * @brief Charles Schwab Market Data API manager (/marketdata/v1/quotes).
 *
 * Provides single and batch quote queries with pre-flight parameter validation,
 * OAuth Bearer header injection, and conversion into 64-byte Quote structs.
 */
class MarketDataManager {
public:
    MarketDataManager() = default;
    explicit MarketDataManager(HttpClient* client, std::string access_token = {}) noexcept
        : client_(client), access_token_(std::move(access_token)) {}

    void set_client(HttpClient* client) noexcept { client_ = client; }
    void set_access_token(std::string_view token) { access_token_ = std::string(token); }
    [[nodiscard]] HttpClient* client() const noexcept { return client_; }
    [[nodiscard]] std::string_view access_token() const noexcept { return access_token_; }

    /**
     * @brief Fetches single real-time quote for symbol using bound client and token.
     */
    [[nodiscard]] std::optional<Quote> get_quote(
        std::string_view symbol,
        std::string* error_msg = nullptr
    ) const;

    /**
     * @brief Fetches real-time quotes for multiple symbols using bound client and token.
     */
    [[nodiscard]] std::vector<Quote> get_quotes(
        std::span<const Symbol> symbols,
        std::string* error_msg = nullptr
    ) const;

    /**
     * @brief Fetches real-time quotes for multiple symbols (string_view span).
     */
    [[nodiscard]] std::vector<Quote> get_quotes(
        std::span<const std::string_view> symbols,
        std::string* error_msg = nullptr
    ) const;

    // --- Static Stateless API ---

    /**
     * @brief Fetches single quote via Charles Schwab Market Data API.
     *
     * Queries /marketdata/v1/{symbol}/quotes.
     *
     * @param client Active HttpClient instance.
     * @param symbol Ticker symbol to query (e.g. "AAPL").
     * @param access_token Valid OAuth Bearer token.
     * @param error_msg Optional output pointer for failure diagnostics.
     * @return Populated Quote or std::nullopt on failure.
     */
    [[nodiscard]] static std::optional<Quote> fetch_quote(
        HttpClient& client,
        std::string_view symbol,
        std::string_view access_token,
        std::string* error_msg = nullptr
    );

    /**
     * @brief Fetches multiple quotes in single batch request.
     *
     * Queries /marketdata/v1/quotes?symbols={s1,s2,...}.
     *
     * @param client Active HttpClient instance.
     * @param symbols Collection of ticker symbols to query.
     * @param access_token Valid OAuth Bearer token.
     * @param error_msg Optional output pointer for failure diagnostics.
     * @return Vector of parsed Quotes.
     */
    [[nodiscard]] static std::vector<Quote> fetch_quotes_batch(
        HttpClient& client,
        std::span<const Symbol> symbols,
        std::string_view access_token,
        std::string* error_msg = nullptr
    );

    /**
     * @brief Fetches multiple quotes in single batch request (string_view symbols).
     */
    [[nodiscard]] static std::vector<Quote> fetch_quotes_batch(
        HttpClient& client,
        std::span<const std::string_view> symbols,
        std::string_view access_token,
        std::string* error_msg = nullptr
    );

    /**
     * @brief Fetches multiple quotes using comma-delimited symbol string.
     *
     * Queries /marketdata/v1/quotes?symbols={comma_delimited_symbols}.
     *
     * @param client Active HttpClient instance.
     * @param comma_delimited_symbols Comma-separated symbols (e.g. "AAPL,MSFT,SPY").
     * @param access_token Valid OAuth Bearer token.
     * @param error_msg Optional output pointer for failure diagnostics.
     * @return Vector of parsed Quotes.
     */
    [[nodiscard]] static std::vector<Quote> fetch_quotes_csv(
        HttpClient& client,
        std::string_view comma_delimited_symbols,
        std::string_view access_token,
        std::string* error_msg = nullptr
    );

private:
    HttpClient* client_{nullptr};
    std::string access_token_{};
};

} // namespace schwab

#pragma once

#include "schwab/http_client.hpp"
#include "schwab/order.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace schwab {

/**
 * @brief Live account portfolio balances and positions snapshot from Charles Schwab Trader API.
 */
struct AccountPortfolio {
    std::string account_number{};
    std::string account_hash{};
    std::string account_type{};      ///< e.g. "MARGIN", "CASH"
    double cash_balance{0.0};
    double available_funds{0.0};
    double liquidation_value{0.0};
    std::vector<Position> positions;

    [[nodiscard]] double available_cash() const noexcept {
        if (available_funds > 0.0) {
            return available_funds;
        }
        if (cash_balance > 0.0) {
            return cash_balance;
        }
        return 0.0;
    }
};

/**
 * @brief Charles Schwab account status and position fetching manager.
 *
 * Parses /trader/v1/accounts and /trader/v1/accounts/{accountHash}
 * endpoints to populate account portfolios and positions.
 */
class AccountManager {
public:
    /**
     * @brief Parses account portfolio JSON string into AccountPortfolio.
     *
     * @param json_str Raw JSON response from Schwab Trader API.
     * @param error_msg Optional output pointer for parsing error details.
     * @return Populated AccountPortfolio or std::nullopt on parse failure.
     */
    [[nodiscard]] static std::optional<AccountPortfolio> parse_account_portfolio(
        std::string_view json_str,
        std::string* error_msg = nullptr
    );

    /**
     * @brief Fetches live account positions and cash balance via HttpClient.
     *
     * Queries /trader/v1/accounts/{accountHash}?fields=positions.
     *
     * @param client Active HttpClient instance.
     * @param base_url Schwab API base URL (e.g. "https://api.schwabapi.com").
     * @param account_hash Schwab account hash or account number.
     * @param access_token Valid OAuth Bearer access token.
     * @param error_msg Optional output pointer for network/API error details.
     * @return Populated AccountPortfolio or std::nullopt on error.
     */
    [[nodiscard]] static std::optional<AccountPortfolio> fetch_portfolio(
        HttpClient& client,
        std::string_view base_url,
        std::string_view account_hash,
        std::string_view access_token,
        std::string* error_msg = nullptr
    );

    /**
     * @brief Fetches linked account summaries via /trader/v1/accounts.
     *
     * @param client Active HttpClient instance.
     * @param base_url Schwab API base URL.
     * @param access_token Valid OAuth Bearer access token.
     * @param error_msg Optional output pointer for error details.
     * @return List of AccountPortfolio summaries.
     */
    [[nodiscard]] static std::vector<AccountPortfolio> fetch_linked_accounts(
        HttpClient& client,
        std::string_view base_url,
        std::string_view access_token,
        std::string* error_msg = nullptr
    );
};

} // namespace schwab

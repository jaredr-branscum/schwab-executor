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

    [[nodiscard]] std::string masked_account_number() const {
        if (account_number.length() <= 4) {
            return account_number;
        }
        return std::string("***").append(account_number.substr(account_number.length() - 4));
    }

    [[nodiscard]] std::string masked_account_hash() const {
        if (account_hash.length() <= 8) {
            return account_hash;
        }
        return account_hash.substr(0, 4) + "..." + account_hash.substr(account_hash.length() - 4);
    }
};

/**
 * @brief Mapping from raw Schwab account number to encrypted hashValue.
 */
struct AccountNumberMapping {
    std::string account_number{};
    std::string hash_value{};
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
     * @brief Parses account number to encrypted hash mapping list from JSON.
     *
     * @param json_str Raw JSON response from /trader/v1/accounts/accountNumbers.
     * @param error_msg Optional output pointer for error details.
     * @return List of AccountNumberMapping structs.
     */
    [[nodiscard]] static std::vector<AccountNumberMapping> parse_account_numbers(
        std::string_view json_str,
        std::string* error_msg = nullptr
    );

    /**
     * @brief Fetches account number to encrypted hash mappings via /trader/v1/accounts/accountNumbers.
     *
     * @param client Active HttpClient instance.
     * @param base_url Schwab API base URL.
     * @param access_token Valid OAuth Bearer access token.
     * @param error_msg Optional output pointer for error details.
     * @return List of AccountNumberMapping structs.
     */
    [[nodiscard]] static std::vector<AccountNumberMapping> fetch_account_numbers(
        HttpClient& client,
        std::string_view base_url,
        std::string_view access_token,
        std::string* error_msg = nullptr
    );

    /**
     * @brief Parses multiple linked account portfolios from JSON array.
     *
     * @param json_str Raw JSON response from /trader/v1/accounts?fields=positions.
     * @param error_msg Optional output pointer for error details.
     * @return List of AccountPortfolio structs.
     */
    [[nodiscard]] static std::vector<AccountPortfolio> parse_linked_accounts(
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
     * @brief Fetches linked account summaries via /trader/v1/accounts?fields=positions,
     *        automatically correlating encrypted hashes via /trader/v1/accounts/accountNumbers.
     *
     * @param client Active HttpClient instance.
     * @param base_url Schwab API base URL.
     * @param access_token Valid OAuth Bearer access token.
     * @param error_msg Optional output pointer for error details.
     * @return List of AccountPortfolio summaries with populated account_hash.
     */
    [[nodiscard]] static std::vector<AccountPortfolio> fetch_linked_accounts(
        HttpClient& client,
        std::string_view base_url,
        std::string_view access_token,
        std::string* error_msg = nullptr
    );

    /**
     * @brief Checks if account matches user-specified account filter criteria.
     *
     * Matches "all", full account number, account hash, or last 4 digits of account number.
     * Supports comma-separated filters (e.g. "1234,5678").
     *
     * @param account Target AccountPortfolio.
     * @param filter_str Filter expression ("all", number, or comma-separated tokens).
     * @return true if matching, false otherwise.
     */
    [[nodiscard]] static bool matches_account_filter(
        const AccountPortfolio& account,
        std::string_view filter_str
    ) noexcept;

    /**
     * @brief Filters list of account portfolios according to filter criteria.
     *
     * @param accounts Input portfolios.
     * @param filter_str Filter expression ("all", number, comma-separated tokens).
     * @return Filtered vector of AccountPortfolio.
     */
    [[nodiscard]] static std::vector<AccountPortfolio> filter_accounts(
        const std::vector<AccountPortfolio>& accounts,
        std::string_view filter_str
    );
};

} // namespace schwab

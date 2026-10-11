#include "schwab/account_manager.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <iostream>

namespace schwab {

namespace {

double parse_json_double(const nlohmann::json& j, const char* key, double default_val = 0.0) {
    if (!j.contains(key) || j[key].is_null()) {
        return default_val;
    }
    const auto& v = j[key];
    if (v.is_number()) {
        return v.get<double>();
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        double result = default_val;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
        if (ec == std::errc()) {
            return result;
        }
    }
    return default_val;
}

std::string parse_json_string(const nlohmann::json& j, const char* key, std::string default_val = "") {
    if (!j.contains(key) || j[key].is_null()) {
        return default_val;
    }
    const auto& v = j[key];
    if (v.is_string()) {
        return v.get<std::string>();
    }
    return default_val;
}

AccountPortfolio parse_single_account(const nlohmann::json& sec) {
    AccountPortfolio pf;
    pf.account_number = parse_json_string(sec, "accountNumber");
    pf.account_type = parse_json_string(sec, "type", "INDIVIDUAL");

    // Extract Balances
    if (sec.contains("currentBalances") && sec["currentBalances"].is_object()) {
        const auto& bal = sec["currentBalances"];
        pf.cash_balance = parse_json_double(bal, "cashBalance");
        pf.available_funds = parse_json_double(bal, "availableFunds");
        if (pf.available_funds <= 0.0) {
            pf.available_funds = parse_json_double(bal, "buyingPower");
        }
        if (pf.available_funds <= 0.0) {
            pf.available_funds = parse_json_double(bal, "cashAvailableForTrading");
        }
        pf.liquidation_value = parse_json_double(bal, "liquidationValue");
    }

    // Extract Positions
    if (sec.contains("positions") && sec["positions"].is_array()) {
        const auto& pos_arr = sec["positions"];
        pf.positions.reserve(pos_arr.size());
        for (const auto& item : pos_arr) {
            if (!item.is_object()) {
                continue;
            }

            std::string sym_str;
            if (item.contains("instrument") && item["instrument"].is_object()) {
                sym_str = parse_json_string(item["instrument"], "symbol");
            }
            if (sym_str.empty()) {
                sym_str = parse_json_string(item, "symbol");
            }
            if (sym_str.empty()) {
                continue;
            }

            double long_qty = parse_json_double(item, "longQuantity");
            double short_qty = parse_json_double(item, "shortQuantity");
            double net_qty = long_qty - short_qty;
            if (net_qty == 0.0 && item.contains("settledLongQuantity")) {
                net_qty = parse_json_double(item, "settledLongQuantity");
            }

            double avg_price = parse_json_double(item, "averagePrice");
            double market_val = parse_json_double(item, "marketValue");
            double cur_price = avg_price;
            if (net_qty > 0.0 && market_val > 0.0) {
                cur_price = market_val / net_qty;
            }

            Position pos;
            pos.quantity = net_qty;
            pos.average_price = avg_price;
            pos.current_price = cur_price;
            pos.symbol = Symbol(sym_str);

            if (pos.symbol.is_valid()) {
                pf.positions.push_back(pos);
            }
        }
    }

    return pf;
}

} // namespace

std::optional<AccountPortfolio> AccountManager::parse_account_portfolio(
    std::string_view json_str,
    std::string* error_msg
) {
    if (json_str.empty()) {
        if (error_msg) *error_msg = "Empty JSON response received";
        return std::nullopt;
    }

    try {
        auto root = nlohmann::json::parse(json_str);

        // Case 1: Array of accounts
        if (root.is_array()) {
            if (root.empty()) {
                if (error_msg) *error_msg = "Account list array is empty";
                return std::nullopt;
            }
            const auto& first_acc = root[0];
            if (first_acc.contains("securitiesAccount") && first_acc["securitiesAccount"].is_object()) {
                return parse_single_account(first_acc["securitiesAccount"]);
            }
            return parse_single_account(first_acc);
        }

        // Case 2: Single account object
        if (root.is_object()) {
            if (root.contains("securitiesAccount") && root["securitiesAccount"].is_object()) {
                return parse_single_account(root["securitiesAccount"]);
            }
            return parse_single_account(root);
        }

        if (error_msg) *error_msg = "JSON root is neither object nor array";
        return std::nullopt;
    } catch (const nlohmann::json::exception& e) {
        if (error_msg) *error_msg = std::string("JSON parse error: ") + e.what();
        return std::nullopt;
    }
}

std::optional<AccountPortfolio> AccountManager::fetch_portfolio(
    HttpClient& client,
    std::string_view base_url,
    std::string_view account_hash,
    std::string_view access_token,
    std::string* error_msg
) {
    if (account_hash.empty()) {
        if (error_msg) *error_msg = "Account hash is empty";
        return std::nullopt;
    }
    if (access_token.empty()) {
        if (error_msg) *error_msg = "Access token is empty";
        return std::nullopt;
    }

    std::string path = "/trader/v1/accounts/";
    path.append(account_hash);
    path.append("?fields=positions");

    std::string auth_header = "Bearer ";
    auth_header.append(access_token);

    (void)base_url; // base_url is configured in HttpClient transport
    HttpResponse resp = client.get(path, auth_header);

    if (resp.status_code != 200) {
        if (error_msg) {
            *error_msg = "HTTP " + std::to_string(resp.status_code) + ": " + resp.body;
        }
        return std::nullopt;
    }

    auto pf_opt = parse_account_portfolio(resp.body, error_msg);
    if (pf_opt.has_value()) {
        pf_opt->account_hash = std::string(account_hash);
    }
    return pf_opt;
}

std::vector<AccountPortfolio> AccountManager::fetch_linked_accounts(
    HttpClient& client,
    std::string_view base_url,
    std::string_view access_token,
    std::string* error_msg
) {
    if (access_token.empty()) {
        if (error_msg) *error_msg = "Access token is empty";
        return {};
    }

    std::string path = "/trader/v1/accounts";
    std::string auth_header = "Bearer ";
    auth_header.append(access_token);

    (void)base_url;
    HttpResponse resp = client.get(path, auth_header);

    if (resp.status_code != 200) {
        if (error_msg) {
            *error_msg = "HTTP " + std::to_string(resp.status_code) + ": " + resp.body;
        }
        return {};
    }

    try {
        auto root = nlohmann::json::parse(resp.body);
        std::vector<AccountPortfolio> portfolios;
        if (root.is_array()) {
            portfolios.reserve(root.size());
            for (const auto& item : root) {
                if (item.contains("securitiesAccount") && item["securitiesAccount"].is_object()) {
                    portfolios.push_back(parse_single_account(item["securitiesAccount"]));
                } else if (item.is_object()) {
                    portfolios.push_back(parse_single_account(item));
                }
            }
        }
        return portfolios;
    } catch (const nlohmann::json::exception& e) {
        if (error_msg) *error_msg = std::string("JSON parse error: ") + e.what();
        return {};
    }
}

} // namespace schwab

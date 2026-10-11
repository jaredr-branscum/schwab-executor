#include "schwab/market_data.hpp"

#include <nlohmann/json.hpp>

#include <charconv>

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

uint64_t parse_json_uint64(const nlohmann::json& j, const char* key, uint64_t default_val = 0) {
    if (!j.contains(key) || j[key].is_null()) {
        return default_val;
    }
    const auto& v = j[key];
    if (v.is_number_unsigned()) {
        return v.get<uint64_t>();
    }
    if (v.is_number()) {
        int64_t signed_val = v.get<int64_t>();
        return signed_val > 0 ? static_cast<uint64_t>(signed_val) : 0;
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        uint64_t result = default_val;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
        if (ec == std::errc()) {
            return result;
        }
    }
    return default_val;
}

Quote extract_quote_from_node(const nlohmann::json& node, std::string_view sym_str) {
    Quote q;
    q.symbol = Symbol(sym_str);

    // Support Schwab nested { "quote": { ... } } or flat JSON
    const auto& q_obj = (node.contains("quote") && node["quote"].is_object()) 
        ? node["quote"] 
        : node;

    q.bid_price = parse_json_double(q_obj, "bidPrice");
    q.ask_price = parse_json_double(q_obj, "askPrice");
    q.last_price = parse_json_double(q_obj, "lastPrice");
    q.mark_price = parse_json_double(q_obj, "mark");
    if (q.mark_price <= 0.0) {
        q.mark_price = parse_json_double(q_obj, "markPrice");
    }
    q.total_volume = parse_json_uint64(q_obj, "totalVolume");
    q.quote_time_ms = parse_json_uint64(q_obj, "quoteTime");
    if (q.quote_time_ms == 0) {
        q.quote_time_ms = parse_json_uint64(q_obj, "tradeTime");
    }

    return q;
}

} // namespace

std::optional<Quote> QuoteParser::parse_single(
    std::string_view json_content,
    std::string_view target_symbol
) noexcept {
    if (json_content.empty() || target_symbol.empty()) {
        return std::nullopt;
    }

    try {
        auto j = nlohmann::json::parse(json_content);

        std::string key(target_symbol);
        if (j.contains(key) && j[key].is_object()) {
            Quote q = extract_quote_from_node(j[key], target_symbol);
            if (q.is_valid()) {
                return q;
            }
        }

        // Direct flat object or single symbol response
        if (j.is_object()) {
            if (j.contains("symbol") && j["symbol"].is_string() && j["symbol"].get_ref<const std::string&>() == target_symbol) {
                Quote q = extract_quote_from_node(j, target_symbol);
                if (q.is_valid()) {
                    return q;
                }
            } else if (j.contains("quote") && j["quote"].is_object()) {
                Quote q = extract_quote_from_node(j, target_symbol);
                if (q.is_valid()) {
                    return q;
                }
            }
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

std::vector<Quote> QuoteParser::parse_batch(
    std::string_view json_content
) noexcept {
    if (json_content.empty()) {
        return {};
    }

    std::vector<Quote> results;

    try {
        auto j = nlohmann::json::parse(json_content);

        if (j.is_object()) {
            results.reserve(j.size());
            for (const auto& [key, val] : j.items()) {
                if (!val.is_object()) {
                    continue;
                }
                std::string sym = key;
                if (val.contains("symbol") && val["symbol"].is_string()) {
                    sym = val["symbol"].get<std::string>();
                }
                Quote q = extract_quote_from_node(val, sym);
                if (q.is_valid()) {
                    results.push_back(q);
                }
            }
        } else if (j.is_array()) {
            results.reserve(j.size());
            for (const auto& elem : j) {
                if (!elem.is_object()) {
                    continue;
                }
                std::string sym = elem.value("symbol", "");
                if (sym.empty()) {
                    continue;
                }
                Quote q = extract_quote_from_node(elem, sym);
                if (q.is_valid()) {
                    results.push_back(q);
                }
            }
        }
    } catch (...) {
        return {};
    }

    return results;
}

std::optional<Quote> MarketDataManager::get_quote(
    std::string_view symbol,
    std::string* error_msg
) const {
    if (!client_) {
        if (error_msg) *error_msg = "HttpClient instance is null";
        return std::nullopt;
    }
    return fetch_quote(*client_, symbol, access_token_, error_msg);
}

std::vector<Quote> MarketDataManager::get_quotes(
    std::span<const Symbol> symbols,
    std::string* error_msg
) const {
    if (!client_) {
        if (error_msg) *error_msg = "HttpClient instance is null";
        return {};
    }
    return fetch_quotes_batch(*client_, symbols, access_token_, error_msg);
}

std::vector<Quote> MarketDataManager::get_quotes(
    std::span<const std::string_view> symbols,
    std::string* error_msg
) const {
    if (!client_) {
        if (error_msg) *error_msg = "HttpClient instance is null";
        return {};
    }
    return fetch_quotes_batch(*client_, symbols, access_token_, error_msg);
}

std::optional<Quote> MarketDataManager::fetch_quote(
    HttpClient& client,
    std::string_view symbol,
    std::string_view access_token,
    std::string* error_msg
) {
    if (symbol.empty()) {
        if (error_msg) *error_msg = "Symbol is empty";
        return std::nullopt;
    }
    if (access_token.empty()) {
        if (error_msg) *error_msg = "Access token is empty";
        return std::nullopt;
    }

    std::string path = "/marketdata/v1/";
    path.append(symbol);
    path.append("/quotes");

    std::string auth_header = "Bearer ";
    auth_header.append(access_token);

    HttpResponse resp = client.get(path, auth_header);
    if (resp.status_code != 200) {
        if (error_msg) {
            *error_msg = "HTTP " + std::to_string(resp.status_code) + ": " + resp.body;
        }
        return std::nullopt;
    }

    auto q_opt = QuoteParser::parse_single(resp.body, symbol);
    if (!q_opt.has_value()) {
        if (error_msg) {
            *error_msg = "Failed to parse quote for symbol " + std::string(symbol);
        }
        return std::nullopt;
    }

    return q_opt;
}

std::vector<Quote> MarketDataManager::fetch_quotes_csv(
    HttpClient& client,
    std::string_view comma_delimited_symbols,
    std::string_view access_token,
    std::string* error_msg
) {
    if (comma_delimited_symbols.empty()) {
        if (error_msg) *error_msg = "Symbols parameter is empty";
        return {};
    }
    if (access_token.empty()) {
        if (error_msg) *error_msg = "Access token is empty";
        return {};
    }

    std::string path = "/marketdata/v1/quotes?symbols=";
    path.append(comma_delimited_symbols);

    std::string auth_header = "Bearer ";
    auth_header.append(access_token);

    HttpResponse resp = client.get(path, auth_header);
    if (resp.status_code != 200) {
        if (error_msg) {
            *error_msg = "HTTP " + std::to_string(resp.status_code) + ": " + resp.body;
        }
        return {};
    }

    auto quotes = QuoteParser::parse_batch(resp.body);
    if (quotes.empty() && error_msg) {
        *error_msg = "Failed to parse quotes or empty quote set returned";
    }
    return quotes;
}

std::vector<Quote> MarketDataManager::fetch_quotes_batch(
    HttpClient& client,
    std::span<const Symbol> symbols,
    std::string_view access_token,
    std::string* error_msg
) {
    if (symbols.empty()) {
        if (error_msg) *error_msg = "Symbols list is empty";
        return {};
    }

    std::string csv;
    for (const auto& sym : symbols) {
        if (sym.empty()) continue;
        if (!csv.empty()) csv.push_back(',');
        csv.append(sym.view());
    }

    if (csv.empty()) {
        if (error_msg) *error_msg = "Symbols list contains no valid symbols";
        return {};
    }

    return fetch_quotes_csv(client, csv, access_token, error_msg);
}

std::vector<Quote> MarketDataManager::fetch_quotes_batch(
    HttpClient& client,
    std::span<const std::string_view> symbols,
    std::string_view access_token,
    std::string* error_msg
) {
    if (symbols.empty()) {
        if (error_msg) *error_msg = "Symbols list is empty";
        return {};
    }

    std::string csv;
    for (const auto& sym : symbols) {
        if (sym.empty()) continue;
        if (!csv.empty()) csv.push_back(',');
        csv.append(sym);
    }

    if (csv.empty()) {
        if (error_msg) *error_msg = "Symbols list contains no valid symbols";
        return {};
    }

    return fetch_quotes_csv(client, csv, access_token, error_msg);
}

} // namespace schwab

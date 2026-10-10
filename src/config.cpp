#include "schwab/config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace schwab {

namespace {

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.length() != b.length()) {
        return false;
    }
    for (size_t i = 0; i < a.length(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view trim_sv(std::string_view str) noexcept {
    size_t start = 0;
    while (start < str.size() && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }
    size_t end = str.size();
    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }
    return str.substr(start, end - start);
}

} // namespace

bool Config::has_param(std::string_view key) const noexcept {
    return custom_params.find(key) != custom_params.end();
}

std::string_view Config::get_string(std::string_view key, std::string_view default_val) const noexcept {
    auto it = custom_params.find(key);
    if (it != custom_params.end()) {
        return it->second;
    }
    return default_val;
}

double Config::get_double(std::string_view key, double default_val) const noexcept {
    auto it = custom_params.find(key);
    if (it == custom_params.end()) {
        return default_val;
    }
    std::string_view trimmed = trim_sv(it->second);
    if (trimmed.empty()) {
        return default_val;
    }
    double result = default_val;
    auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), result);
    return (ec == std::errc{} && ptr == trimmed.data() + trimmed.size()) ? result : default_val;
}

int64_t Config::get_int(std::string_view key, int64_t default_val) const noexcept {
    auto it = custom_params.find(key);
    if (it == custom_params.end()) {
        return default_val;
    }
    std::string_view trimmed = trim_sv(it->second);
    if (trimmed.empty()) {
        return default_val;
    }
    int64_t result = default_val;
    auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), result);
    return (ec == std::errc{} && ptr == trimmed.data() + trimmed.size()) ? result : default_val;
}

bool Config::get_bool(std::string_view key, bool default_val) const noexcept {
    auto it = custom_params.find(key);
    if (it == custom_params.end()) {
        return default_val;
    }
    std::string_view trimmed = trim_sv(it->second);
    if (iequals(trimmed, "true") || iequals(trimmed, "1") || iequals(trimmed, "yes") || iequals(trimmed, "on")) {
        return true;
    }
    if (iequals(trimmed, "false") || iequals(trimmed, "0") || iequals(trimmed, "no") || iequals(trimmed, "off")) {
        return false;
    }
    return default_val;
}

std::optional<std::string> Config::validate() const {
    if (app_key.empty()) {
        return "SCHWAB_APP_KEY must not be empty.";
    }
    if (callback_url.empty()) {
        return "SCHWAB_CALLBACK_URL must not be empty.";
    }
    if (base_url.empty()) {
        return "SCHWAB_BASE_URL must not be empty.";
    }
    if (execution.max_order_notional <= 0.0) {
        return "execution.max_order_notional must be strictly positive.";
    }
    if (execution.request_timeout_ms == 0) {
        return "execution.request_timeout_ms must be greater than zero.";
    }
    return std::nullopt;
}

} // namespace schwab

#include "schwab/config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>

namespace schwab {

namespace {

[[nodiscard]] const char* safe_getenv(const char* name) noexcept {
#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable: 4996)
#endif
    const char* val = std::getenv(name);
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
    return val;
}

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

Config Config::load_from_string(std::string_view content) {
    Config cfg;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t next_line = content.find('\n', pos);
        std::string_view line = (next_line == std::string_view::npos) 
            ? content.substr(pos) 
            : content.substr(pos, next_line - pos);
        pos = (next_line == std::string_view::npos) ? content.size() : next_line + 1;

        std::string_view trimmed = trim_sv(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        size_t eq_pos = trimmed.find('=');
        if (eq_pos == std::string_view::npos) {
            continue;
        }

        std::string_view key = trim_sv(trimmed.substr(0, eq_pos));
        std::string_view raw_val = trim_sv(trimmed.substr(eq_pos + 1));
        std::string_view val;

        if (raw_val.size() >= 2 && ((raw_val.front() == '"' && raw_val.back() == '"') ||
                                    (raw_val.front() == '\'' && raw_val.back() == '\''))) {
            val = raw_val.substr(1, raw_val.size() - 2);
        } else {
            size_t comment_pos = raw_val.find('#');
            if (comment_pos != std::string_view::npos) {
                raw_val = trim_sv(raw_val.substr(0, comment_pos));
            }
            val = raw_val;
        }

        if (key.empty()) {
            continue;
        }

        if (key == "SCHWAB_APP_KEY") {
            cfg.app_key = std::string(val);
        } else if (key == "SCHWAB_APP_SECRET") {
            cfg.app_secret = std::string(val);
        } else if (key == "SCHWAB_CALLBACK_URL") {
            cfg.callback_url = std::string(val);
        } else if (key == "SCHWAB_BASE_URL") {
            cfg.base_url = std::string(val);
        } else if (key == "SCHWAB_TOKEN_PATH") {
            cfg.token_path = std::filesystem::path(val);
        } else if (key == "SCHWAB_ACCOUNT_NUMBER") {
            cfg.target_account_number = std::string(val);
        } else if (key == "SCHWAB_DRY_RUN") {
            cfg.execution.dry_run = (iequals(val, "true") || iequals(val, "1") || iequals(val, "yes") || iequals(val, "on"));
        } else if (key == "SCHWAB_MAX_ORDER_NOTIONAL") {
            double notional = 0.0;
            auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), notional);
            if (ec == std::errc{}) {
                cfg.execution.max_order_notional = notional;
            }
        } else if (key == "SCHWAB_REQUEST_TIMEOUT_MS") {
            uint32_t to = 0;
            auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), to);
            if (ec == std::errc{}) {
                cfg.execution.request_timeout_ms = to;
            }
        } else {
            cfg.custom_params[std::string(key)] = std::string(val);
        }
    }
    return cfg;
}

void Config::apply_environment_overrides() {
    auto check_env = [](const char* name) noexcept -> const char* {
        const char* val = safe_getenv(name);
        return (val && *val) ? val : nullptr;
    };

    if (const char* v = check_env("SCHWAB_APP_KEY")) {
        app_key = v;
    }
    if (const char* v = check_env("SCHWAB_APP_SECRET")) {
        app_secret = v;
    }
    if (const char* v = check_env("SCHWAB_CALLBACK_URL")) {
        callback_url = v;
    }
    if (const char* v = check_env("SCHWAB_BASE_URL")) {
        base_url = v;
    }
    if (const char* v = check_env("SCHWAB_TOKEN_PATH")) {
        token_path = std::filesystem::path(v);
    }
    if (const char* v = check_env("SCHWAB_ACCOUNT_NUMBER")) {
        target_account_number = v;
    }
    if (const char* v = check_env("SCHWAB_DRY_RUN")) {
        std::string_view sv(v);
        execution.dry_run = (iequals(sv, "true") || iequals(sv, "1") || iequals(sv, "yes") || iequals(sv, "on"));
    }
    if (const char* v = check_env("SCHWAB_MAX_ORDER_NOTIONAL")) {
        std::string_view sv(v);
        double notional = 0.0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), notional);
        if (ec == std::errc{}) {
            execution.max_order_notional = notional;
        }
    }
    if (const char* v = check_env("SCHWAB_REQUEST_TIMEOUT_MS")) {
        std::string_view sv(v);
        uint32_t to = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), to);
        if (ec == std::errc{}) {
            execution.request_timeout_ms = to;
        }
    }
}

Config Config::load_from_file(const std::filesystem::path& env_path) {
    Config cfg;
    std::error_code ec;
    if (std::filesystem::exists(env_path, ec)) {
        std::ifstream file(env_path);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            cfg = load_from_string(content);
        }
    }
    cfg.apply_environment_overrides();
    return cfg;
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

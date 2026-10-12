#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace schwab {

/// @brief Transparent string hasher for zero-allocation std::string_view lookups in unordered_map
struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

/// @brief Pre-trade risk controls and execution parameters
struct ExecutionOptions {
    double max_order_notional{100000.0}; ///< Maximum allowable single-order notional USD value (pre-trade safety check)
    uint32_t request_timeout_ms{5000};   ///< HTTP network request timeout in milliseconds
    bool dry_run{true};                  ///< Simulation mode: validate and price orders without live market submission
};

/// @brief Strongly-typed configuration model for Charles Schwab execution engine
struct Config {
    // Universal Pre-trade Risk & Execution Parameters (Grouped for memory locality)
    ExecutionOptions execution;

    // Charles Schwab API Credentials & Endpoints
    std::string app_key;                                     ///< Charles Schwab App Key (Client ID)
    std::string app_secret;                                  ///< Charles Schwab App Secret
    std::string callback_url{"https://127.0.0.1"};           ///< Registered OAuth redirect URL
    std::string base_url{"https://api.schwabapi.com"};       ///< Schwab API base URL
    std::string accounts{"all"};                             ///< Target Schwab account(s) filter: "all", comma-separated numbers/hashes (configured via SCHWAB_ACCOUNTS)
    std::filesystem::path token_path{".schwab_token.json"};  ///< Path to cached OAuth tokens

    // Dynamic User-Configurable Strategy Parameters
    // Uses C++20 transparent hashing for zero-allocation std::string_view lookups
    using ParamMap = std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>>;
    ParamMap custom_params;

    /// @brief Retrieve a floating-point parameter with a fallback default (noexcept, zero-allocation)
    [[nodiscard]] double get_double(std::string_view key, double default_val) const noexcept;

    /// @brief Retrieve an integer parameter with a fallback default (noexcept, zero-allocation)
    [[nodiscard]] int64_t get_int(std::string_view key, int64_t default_val) const noexcept;

    /// @brief Retrieve a string parameter view with a fallback default (noexcept, zero-allocation)
    [[nodiscard]] std::string_view get_string(std::string_view key, std::string_view default_val = {}) const noexcept;

    /// @brief Retrieve a boolean parameter with a fallback default (noexcept, zero-allocation)
    [[nodiscard]] bool get_bool(std::string_view key, bool default_val) const noexcept;

    /// @brief Check if a parameter exists in custom_params (noexcept, zero-allocation)
    [[nodiscard]] bool has_param(std::string_view key) const noexcept;

    /// @brief Parse configuration from raw .env text string
    [[nodiscard]] static Config load_from_string(std::string_view content);

    /// @brief Load configuration from .env file with OS environment variable overrides
    [[nodiscard]] static Config load_from_file(const std::filesystem::path& env_path = ".env");

    /// @brief Apply OS environment variable overrides (e.g. std::getenv)
    void apply_environment_overrides();

    /// @brief Validates configuration integrity and pre-trade risk bounds
    /// @return std::nullopt if valid, or a descriptive error message
    [[nodiscard]] std::optional<std::string> validate() const;

    /// @brief Checks if configuration is logically valid
    [[nodiscard]] bool is_valid() const noexcept {
        return !validate().has_value();
    }
};

} // namespace schwab

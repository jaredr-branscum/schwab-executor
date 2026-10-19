#pragma once

#include "schwab/token.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace schwab {

/**
 * @brief Thread-safe token file persistence and JSON deserializer for Charles Schwab OAuth 2.0.
 *
 * Informative diagnostic logging on malformed JSON or filesystem errors.
 * Automatically pre-bakes the Authorization header buffer so loaded tokens
 * are immediately ready for zero-allocation execution on the hot path.
 */
class TokenManager {
public:
    /**
     * @brief Deserializes token JSON string into an OAuthToken.
     *
     * @param json_content Raw JSON string (e.g. from .schwab_token.json).
     * @return Populated and pre-baked OAuthToken, or std::nullopt if parsing fails.
     */
    static std::optional<OAuthToken> load_from_json(std::string_view json_content);

    /**
     * @brief Reads token file from filesystem and delegates to load_from_json.
     *
     * @param path Path to .schwab_token.json file.
     * @return Populated and pre-baked OAuthToken, or std::nullopt if file is missing/unreadable.
     */
    static std::optional<OAuthToken> load_from_file(const std::filesystem::path& path);

    /**
     * @brief Checks if valid, unexpired refresh token exists at specified path.
     *
     * @param path Path to token file (default: ".schwab_token.json").
     * @return true if token file exists and refresh token is valid.
     */
    static bool is_authenticated(const std::filesystem::path& path = ".schwab_token.json");

    /**
     * @brief Ensures active authenticated session exists.
     * If unauthenticated, launches interactive Python login flow.
     *
     * @param path Path to token file (default: ".schwab_token.json").
     * @param python_auth_script Path to python auth helper (default: "python/auth_cli.py").
     * @param force_reauth If true, bypasses existing token cache and launches login flow.
     * @return Populated OAuthToken if authenticated, or std::nullopt if login failed.
     */
    static std::optional<OAuthToken> ensure_authenticated(
        const std::filesystem::path& path = ".schwab_token.json",
        const std::string& python_auth_script = "python/auth_cli.py",
        bool force_reauth = false
    );
};

} // namespace schwab

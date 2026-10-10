#pragma once

#include <string>
#include <string_view>

namespace schwab {

/**
 * @brief Representation of Charles Schwab OAuth 2.0 tokens and validity lifecycle.
 *
 * Encapsulates access and refresh tokens, epoch expiration timestamps,
 * and pre-allocated Authorization header buffer
 */
struct OAuthToken {
    double expires_at = 0.0;                 // Epoch seconds when access token expires (~30 min)
    double refresh_token_expires_at = 0.0;   // Epoch seconds when refresh token expires (~7 days)

    std::string access_token;
    std::string refresh_token;
    std::string token_type = "Bearer";
    std::string scope;
    std::string id_token;
    std::string cached_auth_header;

    /**
     * @brief Pre-bakes "Bearer <token>" so zero string concatenation occurs at execution time.
     */
    void precompute_auth_header();

    /**
     * @brief Zero-copy view into pre-allocated auth header.
     */
    [[nodiscard]] std::string_view auth_header() const noexcept;

    /**
     * @brief Checks if access token is empty or within buffer_seconds of expiration.
     */
    [[nodiscard]] bool is_access_token_expired(double buffer_seconds = 60.0, double now_override = 0.0) const noexcept;

    /**
     * @brief Checks if refresh token is empty or within buffer_seconds of expiration.
     */
    [[nodiscard]] bool is_refresh_token_expired(double buffer_seconds = 300.0, double now_override = 0.0) const noexcept;
};

} // namespace schwab

#include <catch2/catch_test_macros.hpp>
#include "schwab/token.hpp"

TEST_CASE("OAuthToken default state is expired", "[token]") {
    schwab::OAuthToken token;

    // Unpopulated tokens must always be treated as expired
    REQUIRE(token.is_access_token_expired());
    REQUIRE(token.is_refresh_token_expired());
}

TEST_CASE("OAuthToken access expiration honors safety buffer", "[token]") {
    schwab::OAuthToken token;
    token.access_token = "mock_access_token_abc";
    token.token_type = "Bearer";

    const double current_time = 1000.0;

    // Case 1: Token expires in 50 seconds (at 1050.0)
    token.expires_at = 1050.0;

    // With default 60s buffer: 1000 + 60 >= 1050 -> expired (needs refresh)
    CHECK(token.is_access_token_expired(60.0, current_time));

    // With smaller 30s buffer: 1000 + 30 = 1030 < 1050 -> still valid
    CHECK_FALSE(token.is_access_token_expired(30.0, current_time));

    // Case 2: Token already expired in past (at 950.0)
    token.expires_at = 950.0;
    CHECK(token.is_access_token_expired(0.0, current_time));

    // Case 3: Token far in future (at 2800.0, ~30 min)
    token.expires_at = 2800.0;
    CHECK_FALSE(token.is_access_token_expired(60.0, current_time));
}

TEST_CASE("OAuthToken refresh token expiration honors safety buffer", "[token]") {
    schwab::OAuthToken token;
    token.refresh_token = "mock_refresh_token_xyz";

    const double current_time = 1000.0;

    // Refresh token expires in 200 seconds (at 1200.0)
    token.refresh_token_expires_at = 1200.0;

    // Default buffer is 300s: 1000 + 300 = 1300 >= 1200 -> expired
    CHECK(token.is_refresh_token_expired(300.0, current_time));

    // Buffer of 100s: 1000 + 100 = 1100 < 1200 -> still valid
    CHECK_FALSE(token.is_refresh_token_expired(100.0, current_time));

    // Far in future (7 days = 604,800s):
    token.refresh_token_expires_at = current_time + 604800.0;
    CHECK_FALSE(token.is_refresh_token_expired(300.0, current_time));
}

TEST_CASE("OAuthToken precomputes authorization header for zero-allocation hot path", "[token]") {
    schwab::OAuthToken token;
    token.access_token = "TEST_ACCESS_TOKEN_123";

    // Initial state before precompute
    CHECK(token.auth_header().empty());

    token.precompute_auth_header();
    CHECK(token.auth_header() == "Bearer TEST_ACCESS_TOKEN_123");

    // Clearing token clears header
    token.access_token.clear();
    token.precompute_auth_header();
    CHECK(token.auth_header().empty());
}

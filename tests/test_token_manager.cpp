#include <catch2/catch_test_macros.hpp>
#include "schwab/token_manager.hpp"
#include "schwab/time.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

// RAII helper to temporarily redirect std::cerr and std::cout during intentional negative tests
struct ScopedIoSilencer {
    std::stringstream dummy_err;
    std::stringstream dummy_out;
    std::streambuf* orig_err{nullptr};
    std::streambuf* orig_out{nullptr};

    ScopedIoSilencer() noexcept
        : orig_err(std::cerr.rdbuf(dummy_err.rdbuf())),
          orig_out(std::cout.rdbuf(dummy_out.rdbuf())) {}
    ~ScopedIoSilencer() noexcept {
        std::cerr.rdbuf(orig_err);
        std::cout.rdbuf(orig_out);
    }
};

using ScopedCerrSilencer = ScopedIoSilencer;

} // namespace

TEST_CASE("TokenManager parses valid JSON correctly and precomputes auth header", "[token_manager]") {
    const std::string valid_json = R"({
        "access_token": "ACCESS_TOKEN_ABC123",
        "refresh_token": "REFRESH_TOKEN_XYZ789",
        "token_type": "Bearer",
        "expires_at": 1800000000.0,
        "refresh_token_expires_at": 1800604800.0,
        "scope": "api",
        "id_token": "ID_TOKEN_456"
    })";

    auto token_opt = schwab::TokenManager::load_from_json(valid_json);

    REQUIRE(token_opt.has_value());
    CHECK(token_opt->access_token == "ACCESS_TOKEN_ABC123");
    CHECK(token_opt->refresh_token == "REFRESH_TOKEN_XYZ789");
    CHECK(token_opt->token_type == "Bearer");
    CHECK(token_opt->expires_at == 1800000000.0);
    CHECK(token_opt->refresh_token_expires_at == 1800604800.0);
    CHECK(token_opt->scope == "api");
    CHECK(token_opt->id_token == "ID_TOKEN_456");
    CHECK(token_opt->auth_header() == "Bearer ACCESS_TOKEN_ABC123");
}

TEST_CASE("TokenManager handles corrupted or invalid JSON safely", "[token_manager]") {
    ScopedCerrSilencer silencer;

    // Malformed JSON (syntax error)
    auto corrupted = schwab::TokenManager::load_from_json(R"({"access_token": "incomplete)");
    CHECK_FALSE(corrupted.has_value());

    // Empty string
    auto empty = schwab::TokenManager::load_from_json("");
    CHECK_FALSE(empty.has_value());

    // Plain text non-JSON
    auto plain_text = schwab::TokenManager::load_from_json("plain text not json");
    CHECK_FALSE(plain_text.has_value());
}

TEST_CASE("TokenManager loads from filesystem correctly", "[token_manager]") {
    // Non-existent file path
    auto missing = schwab::TokenManager::load_from_file("non_existent_mock_file_12345.json");
    CHECK_FALSE(missing.has_value());

    // File round-trip using temporary file
    auto temp_path = std::filesystem::temp_directory_path() / "schwab_test_token_temp.json";

    const std::string valid_json = R"({
        "access_token": "FILE_ACCESS_TOKEN",
        "refresh_token": "FILE_REFRESH_TOKEN",
        "token_type": "Bearer",
        "expires_at": 1750000000.0,
        "refresh_token_expires_at": 1750604800.0
    })";

    {
        std::ofstream out(temp_path);
        out << valid_json;
    }

    auto loaded = schwab::TokenManager::load_from_file(temp_path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->access_token == "FILE_ACCESS_TOKEN");
    CHECK(loaded->auth_header() == "Bearer FILE_ACCESS_TOKEN");

    // Clean up temporary test file
    std::filesystem::remove(temp_path);
}

TEST_CASE("TokenManager is_authenticated accurately reports session state", "[token_manager]") {
    // Missing file -> false
    CHECK_FALSE(schwab::TokenManager::is_authenticated("missing_token_file_xyz.json"));

    auto temp_path = std::filesystem::temp_directory_path() / "schwab_auth_check_temp.json";

    // Expired refresh token -> false
    {
        std::ofstream out(temp_path);
        out << R"({
            "access_token": "TOK",
            "refresh_token": "REF",
            "expires_at": 1000.0,
            "refresh_token_expires_at": 1000.0
        })";
    }
    CHECK_FALSE(schwab::TokenManager::is_authenticated(temp_path));

    // Valid future refresh token -> true
    double future_refresh = schwab::epoch_seconds_now() + 604800.0;
    double future_access = schwab::epoch_seconds_now() + 1800.0;
    {
        std::ofstream out(temp_path);
        out << "{\n"
            << "  \"access_token\": \"VALID_ACCESS\",\n"
            << "  \"refresh_token\": \"VALID_REFRESH\",\n"
            << "  \"expires_at\": " << std::to_string(future_access) << ",\n"
            << "  \"refresh_token_expires_at\": " << std::to_string(future_refresh) << "\n"
            << "}";
    }
    CHECK(schwab::TokenManager::is_authenticated(temp_path));

    // ensure_authenticated returns active token immediately if already valid
    auto token_opt = schwab::TokenManager::ensure_authenticated(temp_path);
    REQUIRE(token_opt.has_value());
    CHECK(token_opt->access_token == "VALID_ACCESS");
    CHECK(token_opt->auth_header() == "Bearer VALID_ACCESS");

    std::filesystem::remove(temp_path);
}

TEST_CASE("TokenManager ensure_authenticated does not return expired token if re-auth fails", "[token_manager]") {
    ScopedCerrSilencer silencer;
    auto temp_path = std::filesystem::temp_directory_path() / "schwab_expired_token_test.json";

    // Expired access token and expired refresh token
    {
        std::ofstream out(temp_path);
        out << R"({
            "access_token": "EXPIRED_ACCESS",
            "refresh_token": "EXPIRED_REFRESH",
            "expires_at": 1000.0,
            "refresh_token_expires_at": 1000.0
        })";
    }

    // Call ensure_authenticated with a non-existent mock script to simulate failed login
    auto res = schwab::TokenManager::ensure_authenticated(temp_path, "non_existent_auth_script_xyz.py");
    CHECK_FALSE(res.has_value());

    std::filesystem::remove(temp_path);
}

TEST_CASE("TokenManager ensure_authenticated with force_reauth invokes login", "[token_manager]") {
    ScopedCerrSilencer silencer;
    auto temp_path = std::filesystem::temp_directory_path() / "schwab_force_auth_test.json";

    double future_refresh = schwab::epoch_seconds_now() + 604800.0;
    double future_access = schwab::epoch_seconds_now() + 1800.0;
    {
        std::ofstream out(temp_path);
        out << "{\n"
            << "  \"access_token\": \"VALID_ACCESS\",\n"
            << "  \"refresh_token\": \"VALID_REFRESH\",\n"
            << "  \"expires_at\": " << std::to_string(future_access) << ",\n"
            << "  \"refresh_token_expires_at\": " << std::to_string(future_refresh) << "\n"
            << "}";
    }

    // With force_reauth = true and a failing script, it must attempt re-auth rather than returning the cached valid token
    auto res = schwab::TokenManager::ensure_authenticated(temp_path, "non_existent_auth_script_xyz.py", true);
    CHECK_FALSE(res.has_value());

    std::filesystem::remove(temp_path);
}

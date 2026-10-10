#include <catch2/catch_test_macros.hpp>
#include "schwab/token_manager.hpp"

#include <filesystem>
#include <fstream>

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

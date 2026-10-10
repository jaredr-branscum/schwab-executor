#include "schwab/token_manager.hpp"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace schwab {

std::optional<OAuthToken> TokenManager::load_from_json(std::string_view json_content) {
    if (json_content.empty()) {
        return std::nullopt;
    }

    try {
        auto j = nlohmann::json::parse(json_content);

        OAuthToken token;
        token.access_token = j.value("access_token", "");
        token.refresh_token = j.value("refresh_token", "");
        token.token_type = j.value("token_type", "Bearer");
        token.expires_at = j.value("expires_at", 0.0);
        token.refresh_token_expires_at = j.value("refresh_token_expires_at", 0.0);
        token.scope = j.value("scope", "");
        token.id_token = j.value("id_token", "");

        // precompute "Bearer <token>" header buffer
        token.precompute_auth_header();

        return token;
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[ERROR] Failed to parse OAuth token JSON: " << e.what() << "\n";
        return std::nullopt;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Unexpected error parsing OAuth token: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::optional<OAuthToken> TokenManager::load_from_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Unable to open token file: " << path.string() << "\n";
        return std::nullopt;
    }

    // Read entire file into string buffer
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return load_from_json(content);
}

} // namespace schwab

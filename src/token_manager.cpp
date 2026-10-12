#include "schwab/token_manager.hpp"
#include "schwab/process_runner.hpp"

#include <cstdlib>
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

namespace {

[[nodiscard]] std::filesystem::path resolve_token_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec)) {
        return path;
    }

    // Search sibling repository path (e.g. ../schwab-executor/.schwab_token.json)
    if (path.is_relative()) {
        std::filesystem::path sibling = std::filesystem::path("..") / "schwab-executor" / path;
        if (std::filesystem::exists(sibling, ec) && std::filesystem::is_regular_file(sibling, ec)) {
            return sibling;
        }
    }

    return path;
}

[[nodiscard]] std::string resolve_auth_script(const std::string& script) {
    std::error_code ec;
    if (std::filesystem::exists(script, ec) && std::filesystem::is_regular_file(script, ec)) {
        return script;
    }

    // Check sibling repository: ../schwab-executor/python/auth_cli.py
    std::filesystem::path sibling = std::filesystem::path("..") / "schwab-executor" / script;
    if (std::filesystem::exists(sibling, ec) && std::filesystem::is_regular_file(sibling, ec)) {
        return sibling.string();
    }

    // Check ../python/auth_cli.py
    std::filesystem::path parent = std::filesystem::path("..") / script;
    if (std::filesystem::exists(parent, ec) && std::filesystem::is_regular_file(parent, ec)) {
        return parent.string();
    }

    return script;
}

} // namespace

std::optional<OAuthToken> TokenManager::load_from_file(const std::filesystem::path& path) {
    std::filesystem::path actual_path = resolve_token_file(path);
    std::error_code ec;
    if (!std::filesystem::exists(actual_path, ec) || !std::filesystem::is_regular_file(actual_path, ec)) {
        return std::nullopt;
    }

    std::ifstream file(actual_path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Unable to open token file: " << actual_path.string() << "\n";
        return std::nullopt;
    }

    // Read entire file into string buffer
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return load_from_json(content);
}

bool TokenManager::is_authenticated(const std::filesystem::path& path) {
    auto token = load_from_file(path);
    if (!token) {
        return false;
    }
    return !token->is_refresh_token_expired();
}

std::optional<OAuthToken> TokenManager::ensure_authenticated(
    const std::filesystem::path& path,
    const std::string& python_auth_script
) {
    std::filesystem::path actual_path = resolve_token_file(path);
    std::string actual_script = resolve_auth_script(python_auth_script);

    auto token = load_from_file(actual_path);
    if (token && !token->is_refresh_token_expired()) {
        // If access token expired but refresh token is valid, attempt auto-refresh
        if (token->is_access_token_expired()) {
            std::string refresh_cmd = "python \"" + actual_script + "\" --refresh";
            auto res = ProcessRunner::execute(refresh_cmd);
            if (res.exit_code == 0) {
                return load_from_file(actual_path);
            }
        } else {
            return token;
        }
    }

    // Interactive login required via console terminal
    std::cout << "\n[AUTH] No active valid session found. Launching Charles Schwab authentication...\n";
    std::string login_cmd = "python \"" + actual_script + "\" --login";
    int exit_code = std::system(login_cmd.c_str());
    if (exit_code != 0) {
        std::cerr << "[ERROR] Charles Schwab authentication was cancelled or failed.\n";
        return std::nullopt;
    }

    return load_from_file(actual_path);
}

} // namespace schwab

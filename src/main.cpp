#include "schwab/token_manager.hpp"
#include "schwab/process_runner.hpp"
#include "schwab/time.hpp"

#include <iostream>
#include <iomanip>
#include <string_view>
#include <filesystem>

namespace {

void print_help() noexcept {
    std::cout << "\n================================================================================\n"
              << "  Charles Schwab Low-Latency C++ Execution Engine (schwab-executor)\n"
              << "================================================================================\n"
              << "Usage:\n"
              << "  schwab_executor --mode <mode> [options]\n"
              << "  schwab_executor <command>\n\n"
              << "Modes:\n"
              << "  auth    Verify active OAuth session or launch interactive login\n"
              << "  status  Inspect active session diagnostics and live Schwab account status\n"
              << "  help    Display this help message\n\n"
              << "Options:\n"
              << "  --token-path <path>  Path to .schwab_token.json (default: .schwab_token.json)\n"
              << "  --help, -h           Display this help message\n\n"
              << "Examples:\n"
              << "  schwab_executor --mode auth\n"
              << "  schwab_executor --mode status\n"
              << "================================================================================\n\n";
}

// Zero-allocation streamable wrapper for masking sensitive tokens
struct MaskedView {
    std::string_view str;
    size_t prefix{6};
    size_t suffix{4};
};

std::ostream& operator<<(std::ostream& os, const MaskedView& m) {
    if (m.str.length() <= m.prefix + m.suffix) {
        return os << "***";
    }
    return os << m.str.substr(0, m.prefix) << "..." << m.str.substr(m.str.length() - m.suffix);
}

int handle_auth(const std::filesystem::path& token_path) {
    std::cout << "\n================================================================================\n"
              << "  [SCHWAB-EXECUTOR] OAUTH 2.0 SESSION MANAGER\n"
              << "================================================================================\n";

    auto token_opt = schwab::TokenManager::ensure_authenticated(token_path);
    if (!token_opt.has_value()) {
        std::cerr << "  * Status: [FAILED] Authentication could not be completed.\n\n";
        return 1;
    }

    const auto& token = *token_opt;
    double now = schwab::epoch_seconds_now();
    double access_rem = token.expires_at - now;
    double refresh_rem = (token.refresh_token_expires_at - now) / 86400.0; // in days

    std::cout << "  * Status: [AUTHENTICATED] Session is active and ready for execution.\n"
              << "  * Access Token         : " << MaskedView{token.access_token} << "\n"
              << "  * Access Token TTL     : " << std::fixed << std::setprecision(1) << access_rem << "s (" 
              << (access_rem / 60.0) << " min remaining)\n"
              << "  * Refresh Token TTL    : " << std::fixed << std::setprecision(2) << refresh_rem << " days remaining\n"
              << "  * Hot-Path Auth Header : Pre-computed (" << token.auth_header().length() << " bytes, zero allocation)\n"
              << "================================================================================\n\n";
    return 0;
}

int handle_status(const std::filesystem::path& token_path) {
    std::cout << "\n================================================================================\n"
              << "  [SCHWAB-EXECUTOR] LIVE STATUS & DIAGNOSTICS\n"
              << "================================================================================\n";

    auto token_opt = schwab::TokenManager::load_from_file(token_path);
    if (!token_opt.has_value() || token_opt->is_refresh_token_expired()) {
        std::cerr << "  * Status: [UNAUTHENTICATED] No active session found.\n"
                  << "  * Run 'schwab_executor --mode auth' to authenticate with Charles Schwab.\n\n";
        return 1;
    }

    const auto& token = *token_opt;
    double now = schwab::epoch_seconds_now();
    double access_rem = token.expires_at - now;
    double refresh_rem = (token.refresh_token_expires_at - now) / 86400.0;

    std::cout << "  * OAuth Session Status : [AUTHENTICATED]\n"
              << "  * Access Token State   : " << (token.is_access_token_expired() ? "[EXPIRED - REFRESH NEEDED]" : "[VALID]") << "\n"
              << "  * Access Token TTL     : " << std::fixed << std::setprecision(1) << access_rem << "s remaining\n"
              << "  * Refresh Token TTL    : " << std::fixed << std::setprecision(2) << refresh_rem << " days remaining\n"
              << "  * Pre-baked Auth Header: " << MaskedView{token.auth_header(), 10, 4} << "\n";

    // If python/status_cli.py helper exists, invoke it to query live Schwab accounts
    std::filesystem::path status_script = "python/status_cli.py";
    if (std::filesystem::exists(status_script)) {
        std::cout << "\n  --- Charles Schwab Account Diagnostics ---\n";
        auto res = schwab::ProcessRunner::execute("python python/status_cli.py");
        if (res.exit_code == 0) {
            std::cout << res.output;
        } else {
            std::cerr << "  [WARN] Account query returned code " << res.exit_code << ": " << res.output << "\n";
        }
    } else {
        std::cout << "\n  * (Local python/status_cli.py not configured - C++ engine ready for native HTTP)\n";
    }

    std::cout << "================================================================================\n\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    // Disable standard C/C++ I/O synchronization locks for low-latency console streaming
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string_view mode = "status";
    std::string_view token_path_str = ".schwab_token.json";

    // Zero-allocation CLI argument parsing using string_view slices
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "help") {
            print_help();
            return 0;
        } else if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--token-path" && i + 1 < argc) {
            token_path_str = argv[++i];
        } else if (arg == "auth" || arg == "status") {
            mode = arg;
        }
    }

    std::filesystem::path token_path(token_path_str);

    if (mode == "auth") {
        return handle_auth(token_path);
    } else if (mode == "status") {
        return handle_status(token_path);
    } else {
        std::cerr << "[ERROR] Unknown mode: '" << mode << "'. Use --help for usage.\n";
        return 1;
    }
}

#include <catch2/catch_test_macros.hpp>
#include "schwab/config.hpp"
#include <fstream>

using namespace schwab;

TEST_CASE("Config defaults and validation", "[config]") {
    Config cfg;

    SECTION("Default config requires app_key to be valid") {
        CHECK(!cfg.is_valid());
        auto err = cfg.validate();
        REQUIRE(err.has_value());
        CHECK(err->find("SCHWAB_APP_KEY") != std::string::npos);

        // Setting app_key satisfies defaults
        cfg.app_key = "SAMPLE_KEY_12345";
        CHECK(cfg.is_valid());
        CHECK(!cfg.validate().has_value());
    }

    SECTION("Execution defaults prioritize capital safety") {
        CHECK(cfg.execution.dry_run == true);
        CHECK(cfg.execution.max_order_notional == 100000.0);
        CHECK(cfg.execution.request_timeout_ms == 5000);
        CHECK(cfg.callback_url == "https://127.0.0.1");
        CHECK(cfg.base_url == "https://api.schwabapi.com");
        CHECK(cfg.token_path == ".schwab_token.json");
    }

    SECTION("Pre-trade risk bounds validation") {
        cfg.app_key = "SAMPLE_KEY";

        // Non-positive max order notional fails
        cfg.execution.max_order_notional = 0.0;
        CHECK(!cfg.is_valid());
        cfg.execution.max_order_notional = -500.0;
        CHECK(!cfg.is_valid());
        cfg.execution.max_order_notional = 50000.0;
        CHECK(cfg.is_valid());

        // Zero request timeout fails
        cfg.execution.request_timeout_ms = 0;
        CHECK(!cfg.is_valid());
        cfg.execution.request_timeout_ms = 5000;
        CHECK(cfg.is_valid());

        // Empty callback URL fails
        cfg.callback_url = "";
        CHECK(!cfg.is_valid());
        cfg.callback_url = "https://127.0.0.1";
        CHECK(cfg.is_valid());
    }
}

TEST_CASE("Config dynamic custom parameters store", "[config]") {
    Config cfg;

    SECTION("Retrieving missing parameters falls back to specified defaults") {
        CHECK(!cfg.has_param("NON_EXISTENT_KEY"));
        CHECK(cfg.get_double("NON_EXISTENT_KEY", 42.5) == 42.5);
        CHECK(cfg.get_int("NON_EXISTENT_KEY", 100) == 100);
        CHECK(cfg.get_string("NON_EXISTENT_KEY", "DEFAULT") == "DEFAULT");
        CHECK(cfg.get_bool("NON_EXISTENT_KEY", false) == false);
        CHECK(cfg.get_bool("NON_EXISTENT_KEY", true) == true);
    }

    SECTION("Retrieving defined custom parameters") {
        cfg.custom_params["PARAM_DOUBLE"] = "12.34";
        cfg.custom_params["PARAM_INT"] = "50";
        cfg.custom_params["PARAM_STR"] = "TEST_VALUE";
        cfg.custom_params["FLAG_TRUE"] = "true";
        cfg.custom_params["FLAG_YES"] = "YES";
        cfg.custom_params["FLAG_ONE"] = "1";
        cfg.custom_params["FLAG_FALSE"] = "false";
        cfg.custom_params["FLAG_ZERO"] = "0";

        CHECK(cfg.has_param("PARAM_DOUBLE"));
        CHECK(cfg.get_double("PARAM_DOUBLE", 0.0) == 12.34);
        CHECK(cfg.get_int("PARAM_INT", 0) == 50);
        CHECK(cfg.get_string("PARAM_STR", "") == "TEST_VALUE");
        CHECK(cfg.get_bool("FLAG_TRUE", false) == true);
        CHECK(cfg.get_bool("FLAG_YES", false) == true);
        CHECK(cfg.get_bool("FLAG_ONE", false) == true);
        CHECK(cfg.get_bool("FLAG_FALSE", true) == false);
        CHECK(cfg.get_bool("FLAG_ZERO", true) == false);
    }

    SECTION("Malformed parameter values fall back safely to defaults") {
        cfg.custom_params["BAD_DOUBLE"] = "not_a_number";
        cfg.custom_params["BAD_INT"] = "abc123xyz";
        cfg.custom_params["BAD_BOOL"] = "maybe";

        CHECK(cfg.get_double("BAD_DOUBLE", 0.15) == 0.15);
        CHECK(cfg.get_int("BAD_INT", 42) == 42);
        CHECK(cfg.get_bool("BAD_BOOL", true) == true);
        CHECK(cfg.get_bool("BAD_BOOL", false) == false);
    }
}

TEST_CASE("Config loading from string (.env parser)", "[config]") {
    SECTION("Valid .env formatted content with comments and quotes") {
        std::string_view env_content = 
            "# Charles Schwab API Configuration\n"
            "SCHWAB_APP_KEY=TEST_APP_KEY_123\n"
            "SCHWAB_APP_SECRET=\"SECRET_XYZ_456\"\n"
            "SCHWAB_CALLBACK_URL = https://127.0.0.1/auth  # OAuth redirect endpoint\n"
            "SCHWAB_BASE_URL='https://api.schwabapi.com'\n"
            "SCHWAB_TOKEN_PATH=.my_tokens.json\n"
            "SCHWAB_ACCOUNT_NUMBER=99887766\n"
            "SCHWAB_DRY_RUN=false\n"
            "SCHWAB_MAX_ORDER_NOTIONAL=250000.0\n"
            "SCHWAB_REQUEST_TIMEOUT_MS=3000\n"
            "\n"
            "# Custom User Parameters\n"
            "CUSTOM_THRESHOLD=0.10\n"
            "WINDOW_SIZE=100\n"
            "BENCHMARK_SYMBOL=SPY\n";

        Config cfg = Config::load_from_string(env_content);

        CHECK(cfg.app_key == "TEST_APP_KEY_123");
        CHECK(cfg.app_secret == "SECRET_XYZ_456");
        CHECK(cfg.callback_url == "https://127.0.0.1/auth");
        CHECK(cfg.base_url == "https://api.schwabapi.com");
        CHECK(cfg.token_path == ".my_tokens.json");
        CHECK(cfg.target_account_number == "99887766");
        CHECK(cfg.execution.dry_run == false);
        CHECK(cfg.execution.max_order_notional == 250000.0);
        CHECK(cfg.execution.request_timeout_ms == 3000);

        // Custom user parameters
        CHECK(cfg.get_double("CUSTOM_THRESHOLD", 0.0) == 0.10);
        CHECK(cfg.get_int("WINDOW_SIZE", 0) == 100);
        CHECK(cfg.get_string("BENCHMARK_SYMBOL") == "SPY");
        CHECK(cfg.is_valid());
    }

    SECTION("Malformed lines and empty input handled gracefully") {
        std::string_view env_content = 
            "# Comment only\n"
            "\n"
            "INVALID_NO_EQUALS_SIGN\n"
            "   \n"
            "=VALUE_WITHOUT_KEY\n"
            "SCHWAB_APP_KEY=RECOVERED_KEY\n";

        Config cfg = Config::load_from_string(env_content);
        CHECK(cfg.app_key == "RECOVERED_KEY");
        CHECK(cfg.is_valid());
    }
}

TEST_CASE("Config loading from file", "[config]") {
    SECTION("Non-existent file falls back to defaults safely") {
        Config cfg = Config::load_from_file("non_existent_config_file_12345.env");
        CHECK(cfg.app_key.empty());
        CHECK(cfg.execution.dry_run == true);
        CHECK(cfg.execution.max_order_notional == 100000.0);
    }

    SECTION("Reading valid temporary .env file") {
        std::filesystem::path temp_env = "test_temp_config.env";
        {
            std::ofstream out(temp_env);
            out << "SCHWAB_APP_KEY=FILE_LOADED_KEY\n";
            out << "SCHWAB_DRY_RUN=true\n";
            out << "CUSTOM_FLOAT=123.456\n";
        }

        Config cfg = Config::load_from_file(temp_env);
        std::filesystem::remove(temp_env);

        CHECK(cfg.app_key == "FILE_LOADED_KEY");
        CHECK(cfg.execution.dry_run == true);
        CHECK(cfg.get_double("CUSTOM_FLOAT", 0.0) == 123.456);
        CHECK(cfg.is_valid());
    }
}

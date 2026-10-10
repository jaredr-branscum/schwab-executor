#include <catch2/catch_test_macros.hpp>
#include "schwab/config.hpp"

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
        CHECK(!cfg.has_param("VOL_CAP"));
        CHECK(cfg.get_double("VOL_CAP", 0.30) == 0.30);
        CHECK(cfg.get_int("MAX_TRADES", 5) == 5);
        CHECK(cfg.get_string("TICKER", "SPY") == "SPY");
        CHECK(cfg.get_bool("ENABLE_FEATURE", false) == false);
        CHECK(cfg.get_bool("ENABLE_FEATURE", true) == true);
    }

    SECTION("Retrieving defined custom parameters") {
        cfg.custom_params["VOL_CAP"] = "0.26";
        cfg.custom_params["MAX_TRADES"] = "10";
        cfg.custom_params["TICKER"] = "QQQ";
        cfg.custom_params["FLAG_TRUE"] = "true";
        cfg.custom_params["FLAG_YES"] = "YES";
        cfg.custom_params["FLAG_ONE"] = "1";
        cfg.custom_params["FLAG_FALSE"] = "false";
        cfg.custom_params["FLAG_ZERO"] = "0";

        CHECK(cfg.has_param("VOL_CAP"));
        CHECK(cfg.get_double("VOL_CAP", 0.0) == 0.26);
        CHECK(cfg.get_int("MAX_TRADES", 0) == 10);
        CHECK(cfg.get_string("TICKER", "") == "QQQ");
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

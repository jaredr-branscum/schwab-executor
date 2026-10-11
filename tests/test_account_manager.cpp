#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "schwab/account_manager.hpp"

using namespace schwab;

TEST_CASE("AccountManager parses single securitiesAccount JSON with positions", "[account_manager]") {
    const std::string json_str = R"({
        "securitiesAccount": {
            "type": "MARGIN",
            "accountNumber": "12345678",
            "roundTrips": 0,
            "isDayTrader": false,
            "currentBalances": {
                "cashBalance": 25000.50,
                "availableFunds": 25000.50,
                "buyingPower": 50000.00,
                "liquidationValue": 125000.50
            },
            "positions": [
                {
                    "shortQuantity": 0.0,
                    "averagePrice": 450.25,
                    "longQuantity": 100.0,
                    "settledLongQuantity": 100.0,
                    "instrument": {
                        "assetType": "EQUITY",
                        "symbol": "SPY"
                    },
                    "marketValue": 46000.00
                },
                {
                    "shortQuantity": 0.0,
                    "averagePrice": 150.00,
                    "longQuantity": 50.0,
                    "instrument": {
                        "assetType": "EQUITY",
                        "symbol": "AAPL"
                    },
                    "marketValue": 7500.00
                }
            ]
        }
    })";

    std::string err;
    auto pf = AccountManager::parse_account_portfolio(json_str, &err);
    REQUIRE(pf.has_value());
    CHECK(pf->account_number == "12345678");
    CHECK(pf->account_type == "MARGIN");
    CHECK(pf->cash_balance == Catch::Approx(25000.50));
    CHECK(pf->available_funds == Catch::Approx(25000.50));
    CHECK(pf->liquidation_value == Catch::Approx(125000.50));
    CHECK(pf->available_cash() == Catch::Approx(25000.50));

    REQUIRE(pf->positions.size() == 2);
    CHECK(pf->positions[0].symbol == "SPY");
    CHECK(pf->positions[0].quantity == Catch::Approx(100.0));
    CHECK(pf->positions[0].average_price == Catch::Approx(450.25));
    CHECK(pf->positions[0].current_price == Catch::Approx(460.00));

    CHECK(pf->positions[1].symbol == "AAPL");
    CHECK(pf->positions[1].quantity == Catch::Approx(50.0));
    CHECK(pf->positions[1].average_price == Catch::Approx(150.00));
    CHECK(pf->positions[1].current_price == Catch::Approx(150.00));
}

TEST_CASE("AccountManager parses array-wrapped accounts and string values", "[account_manager]") {
    const std::string json_str = R"([
        {
            "securitiesAccount": {
                "type": "CASH",
                "accountNumber": "98765432",
                "currentBalances": {
                    "cashBalance": "5000.25",
                    "availableFunds": "5000.25",
                    "liquidationValue": "5000.25"
                },
                "positions": []
            }
        }
    ])";

    auto pf = AccountManager::parse_account_portfolio(json_str);
    REQUIRE(pf.has_value());
    CHECK(pf->account_number == "98765432");
    CHECK(pf->account_type == "CASH");
    CHECK(pf->cash_balance == Catch::Approx(5000.25));
    CHECK(pf->available_cash() == Catch::Approx(5000.25));
    CHECK(pf->positions.empty());
}

TEST_CASE("AccountManager handles corrupted or invalid JSON safely", "[account_manager]") {
    SECTION("Empty JSON string") {
        std::string err;
        auto pf = AccountManager::parse_account_portfolio("", &err);
        REQUIRE_FALSE(pf.has_value());
        CHECK_FALSE(err.empty());
    }

    SECTION("Malformed JSON syntax") {
        std::string err;
        auto pf = AccountManager::parse_account_portfolio("{ not valid json }", &err);
        REQUIRE_FALSE(pf.has_value());
        CHECK(err.find("JSON parse error") != std::string::npos);
    }

    SECTION("Empty array of accounts") {
        std::string err;
        auto pf = AccountManager::parse_account_portfolio("[]", &err);
        REQUIRE_FALSE(pf.has_value());
        CHECK(err.find("empty") != std::string::npos);
    }
}

TEST_CASE("AccountManager fetch_portfolio parameter guards", "[account_manager]") {
    HttpClient client;
    std::string err;

    SECTION("Empty account hash fails pre-flight") {
        auto pf = AccountManager::fetch_portfolio(client, "https://api.schwabapi.com", "", "token", &err);
        REQUIRE_FALSE(pf.has_value());
        CHECK(err == "Account hash is empty");
    }

    SECTION("Empty access token fails pre-flight") {
        auto pf = AccountManager::fetch_portfolio(client, "https://api.schwabapi.com", "hash", "", &err);
        REQUIRE_FALSE(pf.has_value());
        CHECK(err == "Access token is empty");
    }
}

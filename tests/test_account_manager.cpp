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

    SECTION("fetch_account_numbers guards on empty access token") {
        auto maps = AccountManager::fetch_account_numbers(client, "https://api.schwabapi.com", "", &err);
        CHECK(maps.empty());
        CHECK(err == "Access token is empty");
    }

    SECTION("fetch_linked_accounts guards on empty access token") {
        auto pfs = AccountManager::fetch_linked_accounts(client, "https://api.schwabapi.com", "", &err);
        CHECK(pfs.empty());
        CHECK(err == "Access token is empty");
    }
}

TEST_CASE("AccountManager parse_account_numbers maps account numbers to hashes", "[account_manager]") {
    const std::string json_str = R"([
        {
            "accountNumber": "11112222",
            "hashValue": "MOCK_HASH_ALPHA_11112222_000011112222333344445555666677778888999900001111"
        },
        {
            "accountNumber": "33334444",
            "hashValue": "MOCK_HASH_BETA_33334444_AAAA9999888877776666555544443333222211110000FFFF"
        }
    ])";

    std::string err;
    auto maps = AccountManager::parse_account_numbers(json_str, &err);
    REQUIRE(maps.size() == 2);
    CHECK(maps[0].account_number == "11112222");
    CHECK(maps[0].hash_value == "MOCK_HASH_ALPHA_11112222_000011112222333344445555666677778888999900001111");
    CHECK(maps[1].account_number == "33334444");
    CHECK(maps[1].hash_value == "MOCK_HASH_BETA_33334444_AAAA9999888877776666555544443333222211110000FFFF");

    SECTION("Handles invalid JSON safely") {
        auto empty_res = AccountManager::parse_account_numbers("", &err);
        CHECK(empty_res.empty());
        CHECK(err == "Empty JSON response received");

        auto malformed = AccountManager::parse_account_numbers("{not json}", &err);
        CHECK(malformed.empty());
        CHECK(err.find("JSON parse error") != std::string::npos);

        auto not_array = AccountManager::parse_account_numbers(R"({"accountNumber": "123"})", &err);
        CHECK(not_array.empty());
        CHECK(err == "Account numbers response is not a JSON array");
    }
}

TEST_CASE("AccountManager parse_linked_accounts parses multiple accounts", "[account_manager]") {
    const std::string json_str = R"([
        {
            "securitiesAccount": {
                "accountNumber": "11112222",
                "type": "INDIVIDUAL",
                "currentBalances": {
                    "cashBalance": 3.68,
                    "availableFunds": 3.68,
                    "liquidationValue": 65586.20
                },
                "positions": [
                    {
                        "instrument": {"symbol": "SPY"},
                        "longQuantity": 150.0,
                        "averagePrice": 120.0,
                        "marketValue": 21000.0
                    }
                ]
            }
        },
        {
            "securitiesAccount": {
                "accountNumber": "33334444",
                "type": "INDIVIDUAL",
                "currentBalances": {
                    "cashBalance": 84979.43,
                    "availableFunds": 84979.43,
                    "liquidationValue": 84979.43
                },
                "positions": []
            }
        }
    ])";

    std::string err;
    auto accounts = AccountManager::parse_linked_accounts(json_str, &err);
    REQUIRE(accounts.size() == 2);

    CHECK(accounts[0].account_number == "11112222");
    CHECK(accounts[0].account_type == "INDIVIDUAL");
    CHECK(accounts[0].cash_balance == Catch::Approx(3.68));
    CHECK(accounts[0].liquidation_value == Catch::Approx(65586.20));
    REQUIRE(accounts[0].positions.size() == 1);
    CHECK(accounts[0].positions[0].symbol == "SPY");
    CHECK(accounts[0].masked_account_number() == "***2222");

    CHECK(accounts[1].account_number == "33334444");
    CHECK(accounts[1].cash_balance == Catch::Approx(84979.43));
    CHECK(accounts[1].positions.empty());
    CHECK(accounts[1].masked_account_number() == "***4444");

    accounts[0].account_hash = "MOCK_HASH_ALPHA_11112222_000011112222333344445555666677778888999900001111";
    CHECK(accounts[0].masked_account_hash() == "MOCK...1111");
}

TEST_CASE("AccountManager matches_account_filter and filter_accounts", "[account_manager]") {
    AccountPortfolio acc1;
    acc1.account_number = "11112222";
    acc1.account_hash = "MOCK_HASH_ALPHA_11112222_000011112222333344445555666677778888999900001111";

    AccountPortfolio acc2;
    acc2.account_number = "33334444";
    acc2.account_hash = "MOCK_HASH_BETA_33334444_AAAA9999888877776666555544443333222211110000FFFF";

    std::vector<AccountPortfolio> accounts = {acc1, acc2};

    SECTION("Empty filter or 'all' matches all accounts") {
        CHECK(AccountManager::matches_account_filter(acc1, ""));
        CHECK(AccountManager::matches_account_filter(acc2, ""));
        CHECK(AccountManager::matches_account_filter(acc1, "all"));
        CHECK(AccountManager::matches_account_filter(acc2, "ALL"));
        CHECK(AccountManager::matches_account_filter(acc1, "All"));

        auto all_matched = AccountManager::filter_accounts(accounts, "all");
        CHECK(all_matched.size() == 2);
    }

    SECTION("Exact account number match") {
        CHECK(AccountManager::matches_account_filter(acc1, "11112222"));
        CHECK_FALSE(AccountManager::matches_account_filter(acc2, "11112222"));

        auto res = AccountManager::filter_accounts(accounts, "11112222");
        REQUIRE(res.size() == 1);
        CHECK(res[0].account_number == "11112222");
    }

    SECTION("Last 4 digits account number suffix match") {
        CHECK(AccountManager::matches_account_filter(acc1, "2222"));
        CHECK_FALSE(AccountManager::matches_account_filter(acc2, "2222"));

        CHECK(AccountManager::matches_account_filter(acc2, "4444"));
        CHECK_FALSE(AccountManager::matches_account_filter(acc1, "4444"));
    }

    SECTION("Exact account hash match") {
        CHECK(AccountManager::matches_account_filter(acc1, "MOCK_HASH_ALPHA_11112222_000011112222333344445555666677778888999900001111"));
        CHECK_FALSE(AccountManager::matches_account_filter(acc2, "MOCK_HASH_ALPHA_11112222_000011112222333344445555666677778888999900001111"));
    }

    SECTION("Comma-separated multi-token filter") {
        CHECK(AccountManager::matches_account_filter(acc1, "2222, 4444"));
        CHECK(AccountManager::matches_account_filter(acc2, "2222, 4444"));

        auto res = AccountManager::filter_accounts(accounts, "2222, 4444");
        CHECK(res.size() == 2);

        auto single = AccountManager::filter_accounts(accounts, "2222, 9999");
        REQUIRE(single.size() == 1);
        CHECK(single[0].account_number == "11112222");
    }

    SECTION("Non-matching filter returns empty") {
        CHECK_FALSE(AccountManager::matches_account_filter(acc1, "99999999"));
        CHECK_FALSE(AccountManager::matches_account_filter(acc2, "0000"));

        auto res = AccountManager::filter_accounts(accounts, "UNKNOWN_ACCOUNT");
        CHECK(res.empty());
    }
}


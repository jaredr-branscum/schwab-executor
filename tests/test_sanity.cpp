#include <catch2/catch_test_macros.hpp>

TEST_CASE("Sanity Check: C++20 Test Harness is operational", "[sanity]") {
    // Verifies fundamental arithmetic and compiler sanity
    REQUIRE(1 + 1 == 2);
    REQUIRE(sizeof(int) == 4);
    
    // Verifies C++ standard string handling
    std::string greeting = "Schwab Executor C++20";
    CHECK(greeting.length() > 0);
    CHECK(greeting.starts_with("Schwab"));
}

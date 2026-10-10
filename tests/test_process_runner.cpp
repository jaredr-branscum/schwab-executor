#include <catch2/catch_test_macros.hpp>
#include "schwab/process_runner.hpp"

TEST_CASE("ProcessRunner executes python command and captures output", "[process]") {
    // Confirm external process spawning works
    auto result = schwab::ProcessRunner::execute("python --version");
    REQUIRE(result.exit_code == 0);
    CHECK(result.output.find("Python") != std::string::npos);
}

TEST_CASE("ProcessRunner captures non-zero exit codes correctly", "[process]") {
    // Run inline python script that exits with code 7
    auto result = schwab::ProcessRunner::execute("python -c \"import sys; sys.exit(7)\"");
    REQUIRE(result.exit_code == 7);
}

TEST_CASE("ProcessRunner executes local python/auth_cli.py helper", "[process]") {
    auto result = schwab::ProcessRunner::execute("python python/auth_cli.py --check");

    // Exit code will be 0 if a valid token is cached, or 1 if unauthenticated
    CHECK((result.exit_code == 0 || result.exit_code == 1));

    bool output_valid = (result.output.find("AUTHENTICATED") != std::string::npos) ||
                        (result.output.find("UNAUTHENTICATED") != std::string::npos);
    CHECK(output_valid);
}

add_executable(run_unit_tests
    test_sanity.cpp
    test_process_runner.cpp
    test_token.cpp
    test_time.cpp
)

target_link_libraries(run_unit_tests PRIVATE 
    schwab_core
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(run_unit_tests)

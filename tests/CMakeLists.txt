add_executable(run_unit_tests
    test_sanity.cpp
)

target_link_libraries(run_unit_tests PRIVATE Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(run_unit_tests)

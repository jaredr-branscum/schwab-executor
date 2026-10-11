add_executable(run_unit_tests
    test_sanity.cpp
    test_process_runner.cpp
    test_token.cpp
    test_time.cpp
    test_token_manager.cpp
    test_config.cpp
    test_indicators.cpp
    test_order.cpp
    test_risk_guard.cpp
    test_http_types.cpp
    test_http_client.cpp
    test_order_serializer.cpp
    test_rebalancer.cpp
    test_execution_driver.cpp
    test_clock.cpp
    test_strategy.cpp
    test_account_manager.cpp
    test_daemon.cpp
)

target_link_libraries(run_unit_tests PRIVATE 
    schwab_core
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(run_unit_tests)

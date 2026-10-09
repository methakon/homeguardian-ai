add_executable(homeguardian_tests
    main.cpp
    core/test_config.cpp
    core/test_logger.cpp
    core/test_application.cpp
    core/test_event.cpp
    core/test_event_validator.cpp
    core/test_simulated_event_source.cpp
    core/test_time_window_correlation.cpp
    core/test_pipeline.cpp
    core/test_alert.cpp
    ../src/core/Config.cpp
    ../src/core/Logger.cpp
    ../src/core/Application.cpp
    ../src/core/EventValidator.cpp
    ../src/core/SimulatedEventSource.cpp
    ../src/core/TimeWindowCorrelation.cpp
    ../src/core/Pipeline.cpp
    ../src/core/UUID.cpp
)

target_include_directories(homeguardian_tests PRIVATE ../src)

target_link_libraries(homeguardian_tests PRIVATE
    Catch2::Catch2
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

include(CTest)
add_test(NAME homeguardian_tests COMMAND homeguardian_tests)

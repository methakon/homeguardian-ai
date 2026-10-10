add_executable(homeguardian_tests
    tests/core/test_config.cpp
    tests/core/test_logger.cpp
    tests/core/test_event.cpp
    tests/core/test_event_validator.cpp
    tests/core/test_simulated_event_source.cpp
    tests/core/test_time_window_correlation.cpp
    tests/core/test_pipeline.cpp
    tests/core/test_alert.cpp
    tests/persistence/test_persistence.cpp
    ../src/core/Config.cpp
    ../src/core/Logger.cpp
    ../src/core/EventValidator.cpp
    ../src/core/SimulatedEventSource.cpp
    ../src/core/TimeWindowCorrelation.cpp
    ../src/core/Pipeline.cpp
    ../src/core/UUID.cpp
    ../src/persistence/SQLiteDatabase.cpp
    ../src/persistence/EventRepository.cpp
    ../src/persistence/AlertRepository.cpp
    ../src/persistence/SchemaManager.cpp
    ../src/persistence/PersistenceManager.cpp
)

target_include_directories(homeguardian_tests PRIVATE ../src ../third_party/sqlite)
target_link_libraries(homeguardian_tests PRIVATE
    Catch2::Catch2WithMain
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    sqlite3_static
)

include(CTest)
add_test(NAME homeguardian_tests COMMAND homeguardian_tests)

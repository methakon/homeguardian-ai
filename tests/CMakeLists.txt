add_executable(homeguardian_tests
    main.cpp
    core/test_config.cpp
    core/test_logger.cpp
    core/test_application.cpp
    ../src/core/Config.cpp
    ../src/core/Logger.cpp
    ../src/core/Application.cpp
)

target_include_directories(homeguardian_tests PRIVATE ../src)

target_link_libraries(homeguardian_tests PRIVATE
    Catch2::Catch2
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

include(CTest)
add_test(NAME homeguardian_tests COMMAND homeguardian_tests)

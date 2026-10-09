cmake_minimum_required(VERSION 3.28.3)
project(homeguardian VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

include(FetchContent)

# Fetch spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
)
FetchContent_MakeAvailable(spdlog)

# Fetch nlohmann_json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

option(HOMEGUARDIAN_BUILD_TESTS "Build tests" ON)

if(HOMEGUARDIAN_BUILD_TESTS)
    enable_testing()
    # Fetch Catch2 v2 to support CATCH_CONFIG_MAIN
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v2.13.10
    )
    FetchContent_MakeAvailable(Catch2)
endif()

add_executable(homeguardian
    src/main.cpp
    src/core/Config.cpp
    src/core/Logger.cpp
    src/core/Application.cpp
)

target_include_directories(homeguardian PRIVATE src)

target_link_libraries(homeguardian PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

install(TARGETS homeguardian DESTINATION bin)

if(HOMEGUARDIAN_BUILD_TESTS)
    add_subdirectory(tests)
endif()

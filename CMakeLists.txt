cmake_minimum_required(VERSION 3.28.3)
project(homeguardian VERSION 1.0.0 LANGUAGES C CXX)

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

# SQLite amalgamation as a C static library (no system headers needed)
add_library(sqlite3_static STATIC
    third_party/sqlite/sqlite3.c
)
target_compile_options(sqlite3_static PRIVATE -DSQLITE_OMIT_SHELL=1 -DSQLITE_OMIT_FTS3=1 -DSQLITE_OMIT_FTS4=1 -DSQLITE_OMIT_FTS5=1 -Wno-unused-parameter)
set_target_properties(sqlite3_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Catch2 header-only with explicit main implementation
add_library(catch2_main STATIC
    third_party/catch2/src/catch_with_main.cpp
)
target_include_directories(catch2_main PRIVATE third_party/catch2/single_include)
target_compile_definitions(catch2_main PRIVATE CATCH_CONFIG_MAIN)
set_target_properties(catch2_main PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

option(HOMEGUARDIAN_BUILD_TESTS "Build tests" ON)
option(HOMEGUARDIAN_BUILD_ANDROID_BACKEND "Cross-compile Android NDK camera/audio backends (requires NDK)" OFF)

if(HOMEGUARDIAN_BUILD_ANDROID_BACKEND)
    # NDK r26d installed side-by-side under the SDK. The Android toolchain file
    # must be supplied by the caller; this target only defines the backend
    # library and links the NDK's camera and aaudio system libraries.
    if(NOT ANDROID)
        message(FATAL_ERROR "HOMEGUARDIAN_BUILD_ANDROID_BACKEND requires an Android toolchain (use the NDK's android.toolchain.cmake)")
    endif()
    add_library(homeguardian_android_backend STATIC
        src/backend/android/NdkCameraDevice.cpp
        src/backend/android/AAudioCaptureDevice.cpp
    )
    target_include_directories(homeguardian_android_backend PRIVATE src src/core)
    target_compile_definitions(homeguardian_android_backend PRIVATE HOMEGUARDIAN_ANDROID=1)
    # NDK camera and aaudio are system libraries provided by the NDK sysroot.
    target_link_libraries(homeguardian_android_backend PRIVATE
        camera2ndk
        aaudio
        mediandk
    )

    # Minimal on-device self-test binary (non-capture only). Exercises the
    # consent gate, device lifecycle, and the fail-closed guards without
    # opening the camera or microphone, so it needs no CAMERA/RECORD_AUDIO
    # permission and is safe to run from `adb shell`.
    add_executable(hg_ondevice_selftest
        src/backend/android/ondevice_selftest.cpp
        src/backend/android/NdkCameraDevice.cpp
        src/backend/android/AAudioCaptureDevice.cpp
        src/core/ConsentGate.cpp
        src/core/ConsentGuardedDevice.cpp
    )
    target_include_directories(hg_ondevice_selftest PRIVATE src src/core ${nlohmann_json_SOURCE_DIR}/include)
    target_compile_definitions(hg_ondevice_selftest PRIVATE HOMEGUARDIAN_ANDROID=1)
    target_link_libraries(hg_ondevice_selftest PRIVATE
        camera2ndk
        aaudio
        mediandk
    )
endif()

if(HOMEGUARDIAN_BUILD_TESTS)
    enable_testing()
endif()

add_executable(homeguardian
    src/main.cpp
    src/core/Config.cpp
    src/core/Logger.cpp
    src/core/Application.cpp
    src/core/EventValidator.cpp
    src/core/SimulatedEventSource.cpp
    src/core/TimeWindowCorrelation.cpp
    src/core/Pipeline.cpp
    src/core/UUID.cpp
    src/core/ConsentGate.cpp
    src/core/SimulatedSensorSource.cpp
    src/core/ConsentGatedAcquisition.cpp
    src/core/ConsentGuardedDevice.cpp
    src/persistence/SQLiteDatabase.cpp
    src/persistence/EventRepository.cpp
    src/persistence/AlertRepository.cpp
    src/persistence/ProfileRepository.cpp
    src/persistence/ConsentRepository.cpp
    src/persistence/RoutineRepository.cpp
    src/persistence/SchemaManager.cpp
    src/persistence/PersistenceManager.cpp
)

target_include_directories(homeguardian PRIVATE src third_party/sqlite)
target_link_libraries(homeguardian PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    sqlite3_static
)

install(TARGETS homeguardian DESTINATION bin)

if(HOMEGUARDIAN_BUILD_TESTS)
    add_executable(homeguardian_tests
        tests/core/test_config.cpp
        tests/core/test_logger.cpp
        tests/core/test_event.cpp
        tests/core/test_event_validator.cpp
        tests/core/test_simulated_event_source.cpp
        tests/core/test_time_window_correlation.cpp
        tests/core/test_pipeline.cpp
        tests/core/test_alert.cpp
        tests/core/test_family_profile.cpp
        tests/core/test_consent.cpp
        tests/core/test_routine.cpp
        tests/core/test_config_e.cpp
        tests/core/test_consent_gate.cpp
        tests/core/test_simulated_sensor.cpp
        tests/core/test_gated_acquisition.cpp
        tests/core/test_consent_guarded_device.cpp
        tests/core/test_fake_device_delivery.cpp
        tests/core/test_camera1_adapter.cpp
        tests/core/test_ubuntu_backend.cpp
        tests/persistence/test_persistence.cpp
        tests/persistence/test_phase_e.cpp
        ../src/core/Config.cpp
        ../src/core/Logger.cpp
        ../src/core/EventValidator.cpp
        ../src/core/SimulatedEventSource.cpp
        ../src/core/TimeWindowCorrelation.cpp
        ../src/core/Pipeline.cpp
        ../src/core/UUID.cpp
        ../src/core/ConsentGate.cpp
        ../src/core/SimulatedSensorSource.cpp
        ../src/core/ConsentGatedAcquisition.cpp
        ../src/core/ConsentGuardedDevice.cpp
        ../src/persistence/SQLiteDatabase.cpp
        ../src/persistence/EventRepository.cpp
        ../src/persistence/AlertRepository.cpp
        ../src/persistence/ProfileRepository.cpp
        ../src/persistence/ConsentRepository.cpp
        ../src/persistence/RoutineRepository.cpp
        ../src/persistence/SchemaManager.cpp
        ../src/persistence/PersistenceManager.cpp
        ../src/backend/linux/SensorDiscovery.cpp
    )

    target_include_directories(homeguardian_tests PRIVATE
        /home/swarna-sekhar-dhar/projects/homeguardian-ai/src
        /home/swarna-sekhar-dhar/projects/homeguardian-ai/third_party/sqlite
        /home/swarna-sekhar-dhar/projects/homeguardian-ai/third_party/catch2/single_include
        /home/swarna-sekhar-dhar/projects/homeguardian-ai/third_party/catch2/single_include/catch2
    )
    target_link_libraries(homeguardian_tests PRIVATE
        catch2_main
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        sqlite3_static
    )

    include(CTest)
    add_test(NAME homeguardian_tests COMMAND homeguardian_tests)
    # Run from the project root so tests that read repo-relative files
    # (e.g. config/homeguardian.json) resolve correctly under ctest.
    set_tests_properties(homeguardian_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

cmake_minimum_required(VERSION 3.28.3)
project(homeguardian VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# Embed the dashboard HTML (single source of truth: src/web/index.html) so the
# native demo binary is self-contained (no Node.js, no web-server file at
# runtime). Generated at BUILD time, with the HTML as a real dependency: an
# incremental `make` after editing the dashboard regenerates the header and
# relinks the bridge. (Configure-time generation left a stale dashboard embedded
# in an already-built binary, which is exactly what the demo would then serve.)
set(HG_WEB_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/generated/hg_web_assets.h")
add_custom_command(
    OUTPUT "${HG_WEB_ASSETS}"
    COMMAND ${CMAKE_COMMAND}
            -DHG_HTML=${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html
            -DHG_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/hg_web_assets.h.in
            -DHG_OUT=${HG_WEB_ASSETS}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_web_assets.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hg_web_assets.h.in"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_web_assets.cmake"
    COMMENT "Embedding the dashboard (src/web/index.html)"
    VERBATIM)

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
    src/core/DetectionFusionRule.cpp
    src/core/ImpactClassifier.cpp
    src/core/SyntheticImpactRule.cpp
    src/core/Pipeline.cpp
    src/core/UUID.cpp
    src/core/ConsentGate.cpp
    src/core/SimulatedSensorSource.cpp
    src/core/ConsentGatedAcquisition.cpp
    src/core/ConsentGuardedDevice.cpp
    src/core/SimulatorRuntime.cpp
    src/core/AlertPolicy.cpp
    src/core/PersistenceAudit.cpp
    src/core/RuntimeStatus.cpp
    src/core/AlexaContract.cpp
    src/core/Observation.cpp
    src/core/PrivacyAuditLog.cpp
    src/core/AnomalyDetector.cpp
    src/core/AnomalyMonitor.cpp
    src/core/DemoScenarios.cpp
    src/core/DemoScenarioRunner.cpp
    src/core/FusionEngine.cpp
    src/core/SyntheticActivityDetector.cpp
    src/core/AlexaCredentialStore.cpp
    src/backend/simulator/SimulatedSensorBackend.cpp
    src/backend/simulator/LogicalDevice.cpp
    src/backend/simulator/AudioBroker.cpp
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

# Local-only NDJSON stdio bridge that exposes the real AlexaContract to the
# JavaScript adapter. No network listener; stdin/stdout only. Used by the
# local end-to-end integration tests and demo (F22-30).
add_executable(hg_alexa_bridge
    ${HG_WEB_ASSETS}          # generated: dependency ordering + rebuild trigger
    src/bridge/hg_alexa_bridge.cpp
    # Sensor/integration status route: the adapter's own config validation and the
    # real (empty) device registry are reported to the dashboard, so these two
    # translation units ARE referenced by the bridge.
    src/core/HomeAssistantAdapter.cpp
    src/core/SecurityObservation.cpp
    src/core/HttpClient.cpp
    src/core/NodeIdentity.cpp   # sanitize_label(), used by the adapter's mappings
    src/core/AlexaContract.cpp
    src/core/AlexaCredentialStore.cpp
    src/core/Application.cpp
    src/core/PersistenceAudit.cpp
    src/core/AlertPolicy.cpp
    src/core/RuntimeStatus.cpp
    src/core/Pipeline.cpp
    src/core/TimeWindowCorrelation.cpp
    src/core/DetectionFusionRule.cpp
    src/core/ImpactClassifier.cpp
    src/core/SyntheticImpactRule.cpp
    src/core/Observation.cpp
    src/core/PrivacyAuditLog.cpp
    src/core/AnomalyDetector.cpp
    src/core/AnomalyMonitor.cpp
    src/core/DemoScenarios.cpp
    src/core/DemoScenarioRunner.cpp
    src/core/FusionEngine.cpp
    src/core/SyntheticActivityDetector.cpp
    src/core/EventValidator.cpp
    src/core/SimulatorRuntime.cpp
    src/core/ConsentGate.cpp
    src/core/ConsentGatedAcquisition.cpp
    src/core/ConsentGuardedDevice.cpp
    src/core/SimulatedEventSource.cpp
    src/core/SimulatedSensorSource.cpp
    src/core/Config.cpp
    src/core/Logger.cpp
    src/core/UUID.cpp
    src/backend/simulator/SimulatedSensorBackend.cpp
    src/backend/simulator/LogicalDevice.cpp
    src/backend/simulator/AudioBroker.cpp
    src/persistence/SQLiteDatabase.cpp
    src/persistence/EventRepository.cpp
    src/persistence/AlertRepository.cpp
    src/persistence/ProfileRepository.cpp
    src/persistence/ConsentRepository.cpp
    src/persistence/RoutineRepository.cpp
    src/persistence/SchemaManager.cpp
    src/persistence/PersistenceManager.cpp
)
target_include_directories(hg_alexa_bridge PRIVATE src third_party/sqlite ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(hg_alexa_bridge PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    sqlite3_static
)

if(HOMEGUARDIAN_BUILD_TESTS)
    add_executable(homeguardian_tests
        ${CMAKE_SOURCE_DIR}/tests/core/test_config.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_logger.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_event.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_event_validator.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_simulated_event_source.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_time_window_correlation.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_pipeline.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_alert.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_family_profile.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_consent.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_routine.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_config_e.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_consent_gate.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_simulated_sensor.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_gated_acquisition.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_consent_guarded_device.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_fake_device_delivery.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_camera1_adapter.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_ubuntu_backend.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_simulator_backend.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_e2e_simulator.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_dual_device_broker.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_runtime_integration.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_event_to_alert.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_alert_policy.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_runtime_hardening.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_persistence_audit.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_alexa_contract.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_alexa_credentials.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_privacy_consent.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_privacy_audit.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_native_http.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_detection.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_demo_scenarios.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_detection_alert.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_anomaly_monitor.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_synthetic_impact.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_node_capture.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_precursor_detection.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_security_sensors.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_ha_adapter.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_observation_integration.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_skill_gaps.cpp
        ${CMAKE_SOURCE_DIR}/tests/persistence/test_persistence.cpp
        ${CMAKE_SOURCE_DIR}/tests/persistence/test_phase_e.cpp
        ${CMAKE_SOURCE_DIR}/tests/persistence/test_integrity.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Config.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Application.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
        ${CMAKE_SOURCE_DIR}/src/core/EventValidator.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SimulatedEventSource.cpp
        ${CMAKE_SOURCE_DIR}/src/core/TimeWindowCorrelation.cpp
        ${CMAKE_SOURCE_DIR}/src/core/DetectionFusionRule.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ImpactClassifier.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SyntheticImpactRule.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CaptureAccounting.cpp
        ${CMAKE_SOURCE_DIR}/src/core/NodeIdentity.cpp
        ${CMAKE_SOURCE_DIR}/src/core/BoundedFeatures.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ActivityDetectors.cpp
        ${CMAKE_SOURCE_DIR}/src/core/DempsterShafer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/PrecursorDetection.cpp
        ${CMAKE_SOURCE_DIR}/src/core/EnvironmentalCorrelation.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SecurityObservation.cpp
        ${CMAKE_SOURCE_DIR}/src/core/HomeAssistantAdapter.cpp
        ${CMAKE_SOURCE_DIR}/src/core/HttpClient.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Pipeline.cpp
        ${CMAKE_SOURCE_DIR}/src/core/UUID.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ConsentGate.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SimulatedSensorSource.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ConsentGatedAcquisition.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ConsentGuardedDevice.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SimulatorRuntime.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AlertPolicy.cpp
        ${CMAKE_SOURCE_DIR}/src/core/PersistenceAudit.cpp
        ${CMAKE_SOURCE_DIR}/src/core/RuntimeStatus.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AlexaContract.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AlexaCredentialStore.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Observation.cpp
        ${CMAKE_SOURCE_DIR}/src/core/PrivacyAuditLog.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AnomalyDetector.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AnomalyMonitor.cpp
        ${CMAKE_SOURCE_DIR}/src/core/DemoScenarios.cpp
        ${CMAKE_SOURCE_DIR}/src/core/DemoScenarioRunner.cpp
        ${CMAKE_SOURCE_DIR}/src/core/FusionEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SyntheticActivityDetector.cpp
        ${CMAKE_SOURCE_DIR}/src/persistence/SQLiteDatabase.cpp
        ${CMAKE_SOURCE_DIR}/src/persistence/EventRepository.cpp
        ${CMAKE_SOURCE_DIR}/src/persistence/AlertRepository.cpp
        ${CMAKE_SOURCE_DIR}/src/persistence/ProfileRepository.cpp
        ${CMAKE_SOURCE_DIR}/src/persistence/ConsentRepository.cpp
        ${CMAKE_SOURCE_DIR}/src/persistence/RoutineRepository.cpp
        ${CMAKE_SOURCE_DIR}/src/persistence/SchemaManager.cpp
        ${CMAKE_SOURCE_DIR}/src/persistence/PersistenceManager.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/linux/SensorDiscovery.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/simulator/SimulatedSensorBackend.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/simulator/LogicalDevice.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/simulator/AudioBroker.cpp
    )

    target_include_directories(homeguardian_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/sqlite
        ${CMAKE_SOURCE_DIR}/third_party/catch2/single_include
        ${CMAKE_SOURCE_DIR}/third_party/catch2/single_include/catch2
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
    # Also expose the source dir so tests that read repo-relative files work
    # when the binary is run directly from an out-of-source build directory.
    set_tests_properties(homeguardian_tests PROPERTIES ENVIRONMENT "HG_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
endif()

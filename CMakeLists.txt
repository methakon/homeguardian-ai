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
        src/backend/android/Camera1Device.cpp
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
        src/backend/android/Camera1Device.cpp
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
# ===========================================================================
# D-03 device ingestion boundary (mutual TLS, separate from the loopback demo)
#
# The protocol, registry, ingest and detector layers above are OpenSSL-free and
# are built and tested on any host. The socket layer is NOT: it requires the
# OpenSSL development headers. When they are absent we say so loudly and skip
# ONLY the two network binaries, so a developer without the toolchain still gets
# the full test suite rather than a confusing compile failure.
# ===========================================================================
find_package(Threads REQUIRED)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "D-03: OpenSSL ${OPENSSL_VERSION} found -> building hg_device_ingress + hg_device_client + hg_ingress_probe")

    set(HG_NET_SOURCES
        src/net/DeviceProtocol.cpp
        src/net/DeviceRegistry.cpp
        src/net/ObservationIngest.cpp
        src/net/ReplayState.cpp
        src/net/IngressConfigPolicy.cpp
        src/net/IngestDetectors.cpp
        src/net/TlsTransport.cpp
        src/net/DeviceIngressServer.cpp
    )
    # The core dependencies of the ingestion path, mirrored from hg_alexa_bridge so
    # the ingress links the SAME validated machinery (one event engine).
    set(HG_INGEST_CORE_SOURCES
        src/core/Observation.cpp
        src/core/SecurityObservation.cpp
        src/core/BoundedFeatures.cpp
        src/core/ActivityDetectors.cpp
        src/core/EnvironmentalCorrelation.cpp
        src/core/AnomalyDetector.cpp
        src/core/AnomalyMonitor.cpp
        src/core/Pipeline.cpp
        src/core/DetectionFusionRule.cpp
        src/core/FusionEngine.cpp
        src/core/AlertPolicy.cpp
        src/core/PersistenceAudit.cpp
        src/core/Logger.cpp
        src/core/EventValidator.cpp
        src/core/UUID.cpp
        src/persistence/SQLiteDatabase.cpp
        src/persistence/SchemaManager.cpp
        src/persistence/AlertRepository.cpp
        src/persistence/EventRepository.cpp
    )

    add_executable(hg_device_ingress
        src/main_ingress.cpp
        ${HG_NET_SOURCES}
        ${HG_INGEST_CORE_SOURCES}
    )
    target_link_libraries(hg_device_ingress PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        sqlite3_static
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    # Include dirs are per-target in this project (there is no global
    # include_directories). Omitting this line compiled nowhere on the host --
    # where the OpenSSL block is skipped -- and only failed inside the container,
    # which is exactly the case a conditional target must be built in before it is
    # claimed to work.
    target_include_directories(hg_device_ingress PRIVATE src third_party/sqlite)
    target_compile_definitions(hg_device_ingress PRIVATE HOMEGUARDIAN_WITH_TLS=1)

    add_executable(hg_device_client
        src/main_device_client.cpp
        src/net/DeviceProtocol.cpp
        src/net/TlsTransport.cpp
    )
    target_link_libraries(hg_device_client PRIVATE
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    target_include_directories(hg_device_client PRIVATE src)
    target_compile_definitions(hg_device_client PRIVATE HOMEGUARDIAN_WITH_TLS=1)

    # hg_ingress_probe : the SERVICE-SPECIFIC health probe for the `ingress` container.
    # The runtime image's HEALTHCHECK talks HTTP to the HUB's loopback API on 8080; the ingress
    # container runs hg_device_ingress on 8443 and nothing else, so it inherited a probe that could
    # never pass and reported "unhealthy" while serving mutual TLS correctly. A plain TCP-connect
    # check would be no better - it passes against any process holding the port. This probe performs
    # a real TLS handshake and verifies the server certificate against the private CA AND the
    # expected name/address (set1_host/set1_ip_asc, TLS 1.2 floor), reporting separately whether the
    # server demanded a client certificate. There is NO way to skip that verification, and it holds
    # no device key, so it authenticates no device - it answers "is the ingress up and is it ours".
    add_executable(hg_ingress_probe
        src/main_ingress_probe.cpp
        src/net/IngressHealthProbe.cpp
    )
    target_link_libraries(hg_ingress_probe PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    target_include_directories(hg_ingress_probe PRIVATE src)
    target_compile_definitions(hg_ingress_probe PRIVATE HOMEGUARDIAN_WITH_TLS=1)
else()
    message(WARNING
        "D-03: OpenSSL development headers not found -> hg_device_ingress and "
        "hg_device_client are NOT built. The protocol, registry, ingest and "
        "detector layers are still built and tested. Install libssl-dev (or run "
        "the container image, which includes it) to build the TLS boundary.")
endif()

# ---------------------------------------------------------------------------
# hg_audio_bridge: the HOST-SIDE, permission-gated audio service (H-01).
#
# It exists so that audio hardware is never reached from a container: the Docker hub keeps
# zero audio privileges and receives only the scalars this process derives. It links the
# real PipeWire-backed capture/playback adapters and the shared audio session, and it
# REFUSES to open the microphone without an explicit operator confirmation flag.
add_executable(hg_audio_bridge
    src/hg_audio_bridge.cpp
    src/audio/AudioActivitySession.cpp
    src/backend/linux/PwAudioCaptureDevice.cpp
    src/backend/linux/LocalSpeaker.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/VoicePhrasebook.cpp
    ${CMAKE_SOURCE_DIR}/src/voice/VoiceResponseController.cpp
    src/backend/linux/SensorDiscovery.cpp
    src/core/BoundedFeatures.cpp
    src/core/ActivityDetectors.cpp
    src/core/AnomalyDetector.cpp
    src/core/AnomalyMonitor.cpp
    src/core/Observation.cpp
    src/core/EventValidator.cpp
    src/core/Logger.cpp
)
target_include_directories(hg_audio_bridge PRIVATE src)
target_link_libraries(hg_audio_bridge PRIVATE
    Threads::Threads
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

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

    # --------------------------------------------------------------------------------
    # hg_mcp_server (HG-MCP-01): MCP over Streamable HTTP.
    #
    # A separate executable on purpose - the existing engine, the web demo and the
    # NDJSON bridge are untouched. It links the same core/persistence units the bridge
    # links, because it builds the same Application, PersistenceAudit and
    # AlexaCredentialStore and exposes them through two read-only tools.
    # --------------------------------------------------------------------------------
    add_executable(hg_mcp_server
        src/main_mcp_server.cpp
        src/mcp/McpProtocol.cpp
        src/mcp/McpTools.cpp
        src/mcp/McpHttpServer.cpp
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
    target_include_directories(hg_mcp_server PRIVATE src third_party/sqlite)
    target_link_libraries(hg_mcp_server PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        sqlite3_static
    )

    add_executable(hg_device_registry_cli
        src/main_device_registry_cli.cpp
        src/net/DeviceRegistry.cpp
    )
    target_include_directories(hg_device_registry_cli PRIVATE src)
    target_link_libraries(hg_device_registry_cli PRIVATE
        nlohmann_json::nlohmann_json
    )

# ---------------------------------------------------------------------------
# hg_site_connector_rehearsal: the site-connector HOST SHAPE, without a deployment.
#
# Mock adapters only: it opens no socket, speaks no camera protocol, carries no media, and
# every observation it produces is labelled synthetic. It exists so the connector chain
# (identity -> reach allowlist -> registry authorization -> per-frame consent -> session
# termination -> control-channel wire -> media-path decision) can be run, measured and
# regression-checked as one executable, and so the resource envelope of a future always-on
# host is a measurement rather than a guess.
#
# Deliberately NOT linked into hg_mcp_server, the web demo or the bridge: camera-connector
# work stays isolated from the MCP demonstration surface.
add_executable(hg_site_connector_rehearsal
    src/main_site_connector_rehearsal.cpp
    src/core/Observation.cpp
    src/net/DeviceRegistry.cpp
    src/camera/CameraInventory.cpp
    src/camera/ControlChannel.cpp
    src/camera/DeviceFingerprint.cpp
    src/camera/DirectMediaPlanner.cpp
    src/camera/MockCameraAdapter.cpp
    src/camera/RegistryAuthorizer.cpp
    src/camera/SiteConnector.cpp
    src/camera/SiteProfile.cpp
)
target_include_directories(hg_site_connector_rehearsal PRIVATE src)
target_link_libraries(hg_site_connector_rehearsal PRIVATE
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# hg_camera_bridge: the REAL camera path on Ubuntu, with an operator gate.
#
# V4L2 is the kernel UAPI, so this needs NO extra library: not OpenCV, not libv4l, not
# PipeWire. Capture requires --confirm; without it the program refuses before opening
# anything. Frames are reduced to bounded luma/motion scalars in place - nothing is written,
# streamed or transmitted - and the finding vocabulary is closed and boring.
#
# Kept out of hg_mcp_server and the web demo on purpose: a real sensor path must not change
# the demonstration surface.
add_executable(hg_camera_bridge
    src/main_camera_bridge.cpp
    src/backend/linux/V4l2CameraDevice.cpp
    src/core/CameraActivitySession.cpp
    src/core/DetectionFusionRule.cpp
    src/core/FusionEngine.cpp
    src/core/ActivityEpisode.cpp
    src/core/ChangeCharacter.cpp
    src/core/UUID.cpp
    src/core/ActivityDetectors.cpp
    src/core/BoundedFeatures.cpp
    src/core/Observation.cpp
)
target_include_directories(hg_camera_bridge PRIVATE src)
target_link_libraries(hg_camera_bridge PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
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
        ${CMAKE_SOURCE_DIR}/tests/core/test_consent_lifecycle.cpp
        ${CMAKE_SOURCE_DIR}/tests/android/test_camera1_device.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_preview_size_policy.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_simulated_sensor.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_gated_acquisition.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_consent_guarded_device.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_fake_device_delivery.cpp
        ${CMAKE_SOURCE_DIR}/src/core/PreviewSizePolicy.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/android/Camera1Device.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_camera1_adapter.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_ubuntu_backend.cpp
        ${CMAKE_SOURCE_DIR}/tests/audio/test_audio_activity_session.cpp
        ${CMAKE_SOURCE_DIR}/tests/voice/test_voice_response.cpp
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
        ${CMAKE_SOURCE_DIR}/tests/core/test_native_http_security.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_native_http_persistence.cpp
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
        ${CMAKE_SOURCE_DIR}/tests/net/test_device_transport.cpp
        # HG-V380-09 camera adapter boundary: deterministic mock adapters plus the
        # registry/consent authorization seam. Registered in the TEST target only —
        # no shipped binary references them, so their footprints are unchanged.
        ${CMAKE_SOURCE_DIR}/src/camera/MockCameraAdapter.cpp
        ${CMAKE_SOURCE_DIR}/src/camera/RegistryAuthorizer.cpp
        ${CMAKE_SOURCE_DIR}/src/camera/SiteConnector.cpp
        ${CMAKE_SOURCE_DIR}/tests/camera/test_camera_boundary.cpp
        ${CMAKE_SOURCE_DIR}/src/camera/SiteProfile.cpp
        ${CMAKE_SOURCE_DIR}/src/camera/ControlChannel.cpp
        ${CMAKE_SOURCE_DIR}/src/camera/DirectMediaPlanner.cpp
        ${CMAKE_SOURCE_DIR}/tests/camera/test_site_connectors.cpp
        ${CMAKE_SOURCE_DIR}/src/camera/DeviceFingerprint.cpp
        ${CMAKE_SOURCE_DIR}/src/camera/CameraInventory.cpp
        ${CMAKE_SOURCE_DIR}/tests/camera/test_camera_inventory.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AnimalActivityScenarios.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_animal_activity_scenarios.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CameraActivitySession.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/linux/V4l2CameraDevice.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_camera_activity.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_camera_calibration.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_camera_replay.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_fusion_provenance.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_activity_episode.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_camera_scenarios.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_animal_activity.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_change_character.cpp
        ${CMAKE_SOURCE_DIR}/tests/core/test_audio_observation.cpp
        ${CMAKE_SOURCE_DIR}/tests/persistence/test_persistence.cpp
        ${CMAKE_SOURCE_DIR}/tests/persistence/test_phase_e.cpp
        ${CMAKE_SOURCE_DIR}/tests/persistence/test_integrity.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Config.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Application.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
        ${CMAKE_SOURCE_DIR}/src/core/EventValidator.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SimulatedEventSource.cpp
        ${CMAKE_SOURCE_DIR}/src/core/TimeWindowCorrelation.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ActivityEpisode.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AnimalActivity.cpp
        ${CMAKE_SOURCE_DIR}/src/core/ChangeCharacter.cpp
        ${CMAKE_SOURCE_DIR}/src/core/AudioObservation.cpp
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
        ${CMAKE_SOURCE_DIR}/src/net/DeviceProtocol.cpp
        ${CMAKE_SOURCE_DIR}/src/net/DeviceRegistry.cpp
        ${CMAKE_SOURCE_DIR}/src/net/ObservationIngest.cpp
        ${CMAKE_SOURCE_DIR}/src/net/ReplayState.cpp
        ${CMAKE_SOURCE_DIR}/src/net/IngressConfigPolicy.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/linux/PwAudioCaptureDevice.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/linux/LocalSpeaker.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/VoicePhrasebook.cpp
        ${CMAKE_SOURCE_DIR}/src/voice/VoiceResponseController.cpp
        ${CMAKE_SOURCE_DIR}/src/audio/AudioActivitySession.cpp
        ${CMAKE_SOURCE_DIR}/src/net/IngestDetectors.cpp
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
        # MCP (HG-MCP-01): protocol, tools and transport, exercised without a socket
        ${CMAKE_SOURCE_DIR}/tests/mcp/test_mcp_protocol.cpp
        ${CMAKE_SOURCE_DIR}/src/mcp/McpProtocol.cpp
        ${CMAKE_SOURCE_DIR}/src/mcp/McpTools.cpp
        ${CMAKE_SOURCE_DIR}/src/mcp/McpHttpServer.cpp
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

    # Evidence provenance: every camera-evidence artifact that a submission document references must
    # resolve at the revision that is supposed to contain it, and the artifact's own content must
    # support the evidence type the manifest declares. Python 3 + the standard library only, so this
    # is skipped rather than failed when no interpreter is present.
    find_program(HG_PYTHON3_EXECUTABLE NAMES python3 python)
    if(HG_PYTHON3_EXECUTABLE)
        add_test(NAME evidence_provenance_validator
                 COMMAND ${HG_PYTHON3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/verify_evidence_provenance.py
                         --root ${CMAKE_SOURCE_DIR}
                         --json-out ${CMAKE_BINARY_DIR}/evidence/provenance-report.json
                         --allow-external-references)
        set_tests_properties(evidence_provenance_validator PROPERTIES
                             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        add_test(NAME evidence_provenance_tests
                 COMMAND ${HG_PYTHON3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests/scripts)
        set_tests_properties(evidence_provenance_tests PROPERTIES
                             WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
endif()

#include "catch2/catch.hpp"
#include "core/SimulatorRuntime.h"
#include "core/Logger.h"

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Runtime integration tests: these drive SimulatorRuntime — the component the
// running Application uses — through the acceptance scenarios. Simulator-only:
// no physical sensor, no DB (in-memory consent repo), deterministic.
// ---------------------------------------------------------------------------

TEST_CASE("Runtime: startup with capture disabled delivers no media",
          "[runtime][consent][disabled]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/false);
    rt.register_default_devices();
    REQUIRE(rt.consumer_count() == 2);

    // Grant consent, but capture is globally disabled -> gate must still deny.
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.start();

    int delivered = rt.tick();
    REQUIRE(delivered == 0);                       // no consumer received media
    REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == 0);
    REQUIRE(rt.delivered_count(SimulatorRuntime::assistant_device_id()) == 0);
    REQUIRE(rt.speaker_played_count() == 0);
    rt.shutdown();
}

TEST_CASE("Runtime: capture enabled + consent delivers to permitted consumers",
          "[runtime][consent][grant]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.start();

    int delivered = rt.tick();
    REQUIRE(delivered == 2);
    REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == 1);
    REQUIRE(rt.delivered_count(SimulatorRuntime::assistant_device_id()) == 1);
    REQUIRE(rt.speaker_played_count() == 2);       // one response per consumer
    REQUIRE(rt.speaker_queue_size() == 0);         // queue drained
    rt.shutdown();
}

TEST_CASE("Runtime: no consent at all -> no media even when capture enabled",
          "[runtime][consent][none]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.start();  // no grants

    int delivered = rt.tick();
    REQUIRE(delivered == 0);
    REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == 0);
    REQUIRE(rt.delivered_count(SimulatorRuntime::assistant_device_id()) == 0);
    REQUIRE(rt.speaker_played_count() == 0);
    rt.shutdown();
}

TEST_CASE("Runtime: withdrawing one consumer's consent blocks only that consumer",
          "[runtime][consent][withdraw]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.start();

    REQUIRE(rt.tick() == 2);  // both delivered
    size_t en_before = rt.delivered_count(SimulatorRuntime::enabled_device_id());
    size_t as_before = rt.delivered_count(SimulatorRuntime::assistant_device_id());

    // Withdraw the assistant only.
    rt.withdraw_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");

    int delivered = rt.tick();
    // Only the enabled device still receives media.
    REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == en_before + 1);
    REQUIRE(rt.delivered_count(SimulatorRuntime::assistant_device_id()) == as_before);
    REQUIRE(delivered == 1);
    rt.shutdown();
}

TEST_CASE("Runtime: multiple cycles are deterministic and memory-bounded",
          "[runtime][bounded]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.start();

    for (int i = 0; i < 10; ++i) {
        REQUIRE(rt.tick() == 2);
    }
    // Speaker queue never grows without bound (drained each cycle).
    REQUIRE(rt.speaker_queue_size() == 0);
    REQUIRE(rt.speaker_played_count() == 20);       // 2 per cycle x 10
    REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == 10);
    REQUIRE(rt.delivered_count(SimulatorRuntime::assistant_device_id()) == 10);
    rt.shutdown();
}

TEST_CASE("Runtime: shutdown is deterministic and idempotent",
          "[runtime][shutdown]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.start();
    REQUIRE(rt.tick() == 1);

    rt.shutdown();
    REQUIRE(rt.shut_down());
    // After shutdown, tick() is a no-op (no further delivery).
    REQUIRE(rt.tick() == 0);
    // Idempotent second shutdown does not crash or double-release.
    rt.shutdown();
    REQUIRE(rt.shut_down());
}

TEST_CASE("Runtime: toggling capture off mid-run blocks subsequent delivery",
          "[runtime][consent][toggle]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.start();
    REQUIRE(rt.tick() == 1);

    // Global kill switch flips OFF at runtime. (set_media_capture_enabled
    // rebuilds the gate + broker, which resets counters — the invariant under
    // test is simply that, once capture is disabled, no media is delivered.)
    rt.set_media_capture_enabled(false);
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.start();

    REQUIRE(rt.tick() == 0);                              // no consumer received media
    REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == 0);
    REQUIRE(rt.speaker_played_count() == 0);              // no playback
    rt.shutdown();
}

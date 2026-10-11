#include "catch2/catch.hpp"
#include "core/AlertPolicy.h"
#include "core/Alert.h"
#include "core/Logger.h"
#include <chrono>
#include <string>
#include <utility>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// AlertPolicy: deduplication + cooldown for repeated alerts from the existing
// correlation engine. Deterministic via an injected clock. Simulator-only.
// ---------------------------------------------------------------------------

namespace {

// A manually-advanced clock for deterministic cooldown/window tests. Held via
// shared_ptr so the policy and the test share the SAME clock instance (the
// Clock std::function would otherwise copy it).
struct FakeClock {
    std::chrono::system_clock::time_point t;
    std::chrono::system_clock::time_point operator()() const { return t; }
    void advance_ms(long long ms) {
        t += std::chrono::milliseconds(ms);
    }
};

Alert make_alert(const std::string& rule, Severity sev, const std::string& msg) {
    return Alert::create("alert-x", sev, msg, {"e1"}, 0.9, rule, nlohmann::json::object());
}

// Convenience: a shared FakeClock (the policy holds a std::function that would
// otherwise COPY the clock, so tests must share one instance to advance time).
std::shared_ptr<FakeClock> make_shared_clock() {
    auto clock = std::make_shared<FakeClock>();
    clock->t = std::chrono::system_clock::now();
    return clock;
}

}  // namespace

TEST_CASE("AlertPolicy: first alert for a key is always delivered",
          "[alertpolicy][first]") {
    Logger::initialize("error");
    FakeClock clock;
    clock.t = std::chrono::system_clock::now();
    AlertPolicy policy(std::chrono::milliseconds(60000), 256, clock);

    bool suppressed = false;
    REQUIRE(policy.admit(make_alert("sim-presence", Severity::info, "m"), suppressed));
    REQUIRE_FALSE(suppressed);
    REQUIRE(policy.delivered_count() == 1);
    REQUIRE(policy.suppressed_count() == 0);
}

TEST_CASE("AlertPolicy: repeated alerts for the same active condition are deduplicated",
          "[alertpolicy][dedup]") {
    Logger::initialize("error");
    FakeClock clock;
    clock.t = std::chrono::system_clock::now();
    AlertPolicy policy(std::chrono::milliseconds(60000), 256, clock);

    // 10 firings of the same condition within cooldown -> 1 delivered, 9 suppressed.
    int delivered = 0, suppressed = 0;
    for (int i = 0; i < 10; ++i) {
        bool sup = false;
        if (policy.admit(make_alert("sim-presence", Severity::info, "m"), sup)) delivered++;
        if (sup) suppressed++;
    }
    REQUIRE(delivered == 1);
    REQUIRE(suppressed == 9);
    REQUIRE(policy.delivered_count() == 1);
    REQUIRE(policy.suppressed_count() == 9);
}

TEST_CASE("AlertPolicy: cooldown expiration re-notifies",
          "[alertpolicy][cooldown]") {
    Logger::initialize("error");
    auto clock = make_shared_clock();
    AlertPolicy policy(std::chrono::milliseconds(60000), 256,
                       [clock] { return clock->operator()(); });

    bool sup = false;
    REQUIRE(policy.admit(make_alert("r", Severity::info, "m"), sup));   // delivered
    REQUIRE_FALSE(policy.admit(make_alert("r", Severity::info, "m"), sup)); // within cooldown
    REQUIRE(sup);

    // Advance past cooldown -> re-notify.
    clock->advance_ms(60001);
    REQUIRE(policy.admit(make_alert("r", Severity::info, "m"), sup));   // delivered again
    REQUIRE_FALSE(sup);
    REQUIRE(policy.delivered_count() == 2);
    REQUIRE(policy.suppressed_count() == 1);
}

TEST_CASE("AlertPolicy: condition clearing re-arms for immediate next alert",
          "[alertpolicy][rearm]") {
    Logger::initialize("error");
    FakeClock clock;
    clock.t = std::chrono::system_clock::now();
    AlertPolicy policy(std::chrono::milliseconds(60000), 256, clock);

    bool sup = false;
    REQUIRE(policy.admit(make_alert("r", Severity::info, "m"), sup));   // delivered
    REQUIRE_FALSE(policy.admit(make_alert("r", Severity::info, "m"), sup)); // suppressed

    // Condition clears.
    policy.mark_condition_cleared("r");

    // Next alert (still within cooldown wall-clock) is a FRESH condition -> delivered.
    REQUIRE(policy.admit(make_alert("r", Severity::info, "m"), sup));
    REQUIRE_FALSE(sup);
    REQUIRE(policy.delivered_count() == 2);
}

TEST_CASE("AlertPolicy: unrelated alert keys are delivered independently",
          "[alertpolicy][independent]") {
    Logger::initialize("error");
    FakeClock clock;
    clock.t = std::chrono::system_clock::now();
    AlertPolicy policy(std::chrono::milliseconds(60000), 256, clock);

    bool sup = false;
    // Two different rules, both within cooldown of their own first alert.
    REQUIRE(policy.admit(make_alert("rule-a", Severity::info, "a"), sup));
    REQUIRE(policy.admit(make_alert("rule-b", Severity::warning, "b"), sup));
    // Same rule again -> suppressed; the other rule still delivers its repeat
    // is also suppressed (its own cooldown), but a NEW key is not affected.
    REQUIRE_FALSE(policy.admit(make_alert("rule-a", Severity::info, "a"), sup));
    REQUIRE(policy.admit(make_alert("rule-c", Severity::critical, "c"), sup));  // new key -> delivered
    REQUIRE(policy.delivered_count() == 3);
    REQUIRE(policy.tracked_keys() == 3);
}

TEST_CASE("AlertPolicy: same rule different severity are independent keys",
          "[alertpolicy][severity]") {
    Logger::initialize("error");
    FakeClock clock;
    clock.t = std::chrono::system_clock::now();
    AlertPolicy policy(std::chrono::milliseconds(60000), 256, clock);

    bool sup = false;
    REQUIRE(policy.admit(make_alert("r", Severity::info, "m"), sup));
    REQUIRE(policy.admit(make_alert("r", Severity::critical, "m"), sup));  // different severity -> delivered
    REQUIRE(policy.tracked_keys() == 2);
    REQUIRE(policy.delivered_count() == 2);
}

TEST_CASE("AlertPolicy: clock moving backward does not extend cooldown or crash",
          "[alertpolicy][clock]") {
    Logger::initialize("error");
    auto clock = make_shared_clock();
    AlertPolicy policy(std::chrono::milliseconds(60000), 256,
                       [clock] { return clock->operator()(); });

    bool sup = false;
    REQUIRE(policy.admit(make_alert("r", Severity::info, "m"), sup));
    // Move clock backward by an hour.
    clock->advance_ms(-3600000);
    // Negative elapsed must be treated as cooldown-expired (deliver), not crash.
    REQUIRE(policy.admit(make_alert("r", Severity::info, "m"), sup));
    REQUIRE(policy.delivered_count() == 2);
}

TEST_CASE("AlertPolicy: retained state is bounded (LRU-style eviction)",
          "[alertpolicy][bounded]") {
    Logger::initialize("error");
    FakeClock clock;
    clock.t = std::chrono::system_clock::now();
    AlertPolicy policy(std::chrono::milliseconds(60000), /*max_tracked_keys=*/4, clock);

    bool sup = false;
    // Generate 20 distinct keys.
    for (int i = 0; i < 20; ++i) {
        policy.admit(make_alert("rule-" + std::to_string(i), Severity::info, "m"), sup);
    }
    // Retained state never exceeds the cap.
    REQUIRE(policy.tracked_keys() <= 4);
    // All 20 first-occurrence alerts were delivered (eviction does not drop a
    // material new alert; it only forgets old dedup state).
    REQUIRE(policy.delivered_count() == 20);
}

TEST_CASE("AlertPolicy: reset clears all state",
          "[alertpolicy][reset]") {
    Logger::initialize("error");
    FakeClock clock;
    clock.t = std::chrono::system_clock::now();
    AlertPolicy policy(std::chrono::milliseconds(60000), 256, clock);

    bool sup = false;
    policy.admit(make_alert("r", Severity::info, "m"), sup);
    policy.admit(make_alert("r", Severity::info, "m"), sup);
    REQUIRE(policy.delivered_count() == 1);
    REQUIRE(policy.suppressed_count() == 1);

    policy.reset();
    REQUIRE(policy.delivered_count() == 0);
    REQUIRE(policy.suppressed_count() == 0);
    REQUIRE(policy.tracked_keys() == 0);
    // After reset, the same alert is a fresh first occurrence -> delivered.
    REQUIRE(policy.admit(make_alert("r", Severity::info, "m"), sup));
    REQUIRE(policy.delivered_count() == 1);
}

TEST_CASE("AlertPolicy: cooldown of 0 delivers every firing (opt-out)",
          "[alertpolicy][nocooldown]") {
    Logger::initialize("error");
    FakeClock clock;
    clock.t = std::chrono::system_clock::now();
    AlertPolicy policy(std::chrono::milliseconds(0), 256, clock);

    bool sup = false;
    int delivered = 0;
    for (int i = 0; i < 5; ++i) {
        if (policy.admit(make_alert("r", Severity::info, "m"), sup)) delivered++;
    }
    REQUIRE(delivered == 5);   // no dedup when cooldown disabled
    REQUIRE(policy.suppressed_count() == 0);
}

TEST_CASE("AlertPolicy: repeated runs with identical inputs are deterministic",
          "[alertpolicy][determinism]") {
    auto run_once = []() {
        FakeClock clock;
        clock.t = std::chrono::system_clock::now();
        AlertPolicy policy(std::chrono::milliseconds(60000), 256, clock);
        bool sup = false;
        int delivered = 0, suppressed = 0;
        for (int i = 0; i < 10; ++i) {
            if (policy.admit(make_alert("sim-presence", Severity::info, "m"), sup)) delivered++;
            if (sup) suppressed++;
            if (i == 4) policy.mark_condition_cleared("sim-presence");  // mid-run clear
        }
        return std::make_pair(delivered, suppressed);
    };
    auto a = run_once();
    auto b = run_once();
    REQUIRE(a == b);
}

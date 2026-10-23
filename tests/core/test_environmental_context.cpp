// Offline functional tests for the environmental context engine (HG-EDGE-05).
//
// Everything here is synthetic and deterministic: no sensor, no network, no device.
// The suite is deliberately adversarial about the one mistake this module exists to
// prevent - letting a device/battery temperature stand in for air temperature.

#include "catch2/catch.hpp"
#include "core/EnvironmentalContext.h"

#include <algorithm>
#include <chrono>
#include <string>

using namespace homeguardian;
using namespace std::chrono;

namespace {

EnvironmentalReading reading(const std::string& channel, double value,
                            system_clock::time_point observed_at,
                            double uncertainty = 0.0, bool declared = false,
                            double quality = 0.9,
                            const std::string& auth = "",
                            const std::string& source = "node:mi-a3-04de") {
    EnvironmentalReading r;
    r.channel = channel;
    r.source = source;
    r.value = value;
    r.observed_at = observed_at;
    r.received_at = observed_at;
    r.provenance = Provenance::synthetic;
    r.quality = quality;
    r.uncertainty = uncertainty;
    r.uncertainty_declared = declared;
    r.authorization = auth;
    return r;
}

const std::string kGrant = "subject=node:mi-a3-04de;purpose=environment_monitor;decision=granted";

}  // namespace

TEST_CASE("Environmental context: channel taxonomy", "[envcontext]") {
    SECTION("atmospheric and device-thermal namespaces are disjoint") {
        REQUIRE(is_atmospheric("atmospheric.temperature"));
        REQUIRE_FALSE(is_device_thermal("atmospheric.temperature"));
        REQUIRE(is_device_thermal("device.battery.temperature"));
        REQUIRE(is_device_thermal("device.soc.temperature"));
        REQUIRE_FALSE(is_atmospheric("device.battery.temperature"));
        REQUIRE_FALSE(is_atmospheric("device.soc.temperature"));
        REQUIRE_FALSE(is_device_thermal("atmospheric.temperature"));

        // The distinction must hold for EVERY channel in the registry, not just the
        // pair we happened to name.
        for (const auto& spec : channel_registry()) {
            if (spec.cls == ChannelClass::atmospheric) {
                REQUIRE_FALSE(is_device_thermal(spec.channel));
            }
            if (spec.cls == ChannelClass::device_thermal) {
                REQUIRE_FALSE(is_atmospheric(spec.channel));
            }
        }
    }

    SECTION("registry has no duplicate channel names") {
        std::vector<std::string> names;
        for (const auto& spec : channel_registry()) names.emplace_back(spec.channel);
        std::sort(names.begin(), names.end());
        REQUIRE(std::adjacent_find(names.begin(), names.end()) == names.end());
    }

    SECTION("unknown channels are refused, not guessed") {
        REQUIRE_FALSE(channel_supported("atmospheric.pressure_at_sea_level"));
        REQUIRE_FALSE(channel_supported("battery.temperature"));   // the tempting wrong name
        REQUIRE(channel_spec("nonsense") == nullptr);
    }

    SECTION("plausibility ranges match the engine's existing environmental bounds") {
        const ChannelSpec* t = channel_spec("atmospheric.temperature");
        REQUIRE(t != nullptr);
        REQUIRE(t->min_value == Approx(-90.0));
        REQUIRE(t->max_value == Approx(70.0));

        const ChannelSpec* h = channel_spec("atmospheric.humidity");
        REQUIRE(h != nullptr);
        REQUIRE(h->min_value == Approx(0.0));
        REQUIRE(h->max_value == Approx(100.0));

        const ChannelSpec* p = channel_spec("atmospheric.pressure");
        REQUIRE(p != nullptr);
        REQUIRE(p->min_value == Approx(850.0));
        REQUIRE(p->max_value == Approx(1100.0));

        const ChannelSpec* l = channel_spec("illuminance");
        REQUIRE(l != nullptr);
        REQUIRE(l->max_value == Approx(200000.0));
    }
}

TEST_CASE("Environmental context: a battery temperature is never air temperature",
          "[envcontext]") {
    EnvironmentalContextEngine engine;
    auto now = system_clock::now();

    // Exactly what the Mi A3 can actually produce: ambient light, motion, and the
    // handset's own thermal telemetry. No atmospheric sensor exists.
    REQUIRE(engine.ingest(reading("illuminance", 96.12, now), now).accepted);
    REQUIRE(engine.ingest(reading("motion.presence", 1.0, now), now).accepted);
    REQUIRE(engine.ingest(reading("device.battery.temperature", 33.5, now), now).accepted);
    REQUIRE(engine.ingest(reading("device.soc.temperature", 46.9, now), now).accepted);

    auto snap = engine.snapshot(now);

    SECTION("the atmospheric series is honestly reported as absent") {
        REQUIRE_FALSE(snap.atmospheric_series_available());
        REQUIRE_FALSE(snap.atmospheric_temperature().has_value());
    }

    SECTION("the battery temperature is available, and separately so") {
        REQUIRE(snap.battery_temperature().has_value());
        REQUIRE(*snap.battery_temperature() == Approx(33.5));
        // It must not appear under the atmospheric channel either.
        REQUIRE_FALSE(snap.has("atmospheric.temperature"));
    }

    SECTION("the two numbers cannot be conflated in the JSON view") {
        const std::string json = snap.to_json();
        REQUIRE(json.find("\"atmospheric_temperature\":null") != std::string::npos);
        REQUIRE(json.find("\"battery_temperature\":33.5") != std::string::npos);
    }

    SECTION("a plausible battery reading still does not satisfy an atmospheric request") {
        // 46.9 C is a perfectly plausible AIR temperature. That is exactly why the
        // channel name, not the plausibility check, has to be the guard.
        EnvironmentalContextEngine soc_only;
        soc_only.ingest(reading("device.soc.temperature", 46.9, now), now);
        auto s2 = soc_only.snapshot(now);
        REQUIRE(s2.has("device.soc.temperature"));
        REQUIRE_FALSE(s2.atmospheric_temperature().has_value());
        REQUIRE_FALSE(s2.atmospheric_series_available());
    }
}

TEST_CASE("Environmental context: validation is fail-closed", "[envcontext]") {
    EnvironmentalContextEngine engine;
    auto now = system_clock::now();

    SECTION("implausible values are refused with a reason") {
        auto r1 = engine.ingest(reading("atmospheric.humidity", 100000.0, now, 0.0, false, 0.9, kGrant), now);
        REQUIRE_FALSE(r1.accepted);
        REQUIRE_FALSE(r1.reason.empty());

        auto r2 = engine.ingest(reading("atmospheric.temperature", -300.0, now, 0.0, false, 0.9, kGrant), now);
        REQUIRE_FALSE(r2.accepted);

        auto r3 = engine.ingest(reading("illuminance", 999999.0, now), now);
        REQUIRE_FALSE(r3.accepted);
    }

    SECTION("non-finite values are refused") {
        auto r = engine.ingest(reading("illuminance", std::nan(""), now), now);
        REQUIRE_FALSE(r.accepted);
    }

    SECTION("a missing timestamp is refused") {
        EnvironmentalReading r = reading("illuminance", 10.0, now);
        r.observed_at = system_clock::time_point{};
        REQUIRE_FALSE(engine.ingest(r, now).accepted);
    }

    SECTION("a future timestamp beyond the allowed skew is refused") {
        auto future = now + minutes(10);
        REQUIRE_FALSE(engine.ingest(reading("illuminance", 10.0, future), now).accepted);
    }

    SECTION("quality outside [0,1] is refused, and negative uncertainty too") {
        REQUIRE_FALSE(engine.ingest(reading("illuminance", 10.0, now, 0.0, false, 1.5), now).accepted);
        REQUIRE_FALSE(engine.ingest(reading("illuminance", 10.0, now, -1.0, true), now).accepted);
    }

    SECTION("nothing is stored on rejection") {
        REQUIRE(engine.reading_count() == 0);
        engine.ingest(reading("atmospheric.humidity", 500.0, now, 0.0, false, 0.9, kGrant), now);
        REQUIRE(engine.reading_count() == 0);
    }
}

TEST_CASE("Environmental context: freshness is computed, not asserted", "[envcontext]") {
    EnvironmentalContextEngine engine;
    auto now = system_clock::now();

    SECTION("a reading inside the channel's max age is fresh") {
        engine.ingest(reading("illuminance", 100.0, now - minutes(1)), now);
        auto snap = engine.snapshot(now);
        const FusedChannel* c = snap.find("illuminance");
        REQUIRE(c != nullptr);
        REQUIRE(c->present);
        REQUIRE(c->freshness == Freshness::fresh);
        REQUIRE(c->age.count() == 60);
    }

    SECTION("a reading past the channel's max age is expired and not present") {
        // illuminance max_age is 5 minutes.
        engine.ingest(reading("illuminance", 100.0, now - minutes(6)), now);
        auto snap = engine.snapshot(now);
        const FusedChannel* c = snap.find("illuminance");
        REQUIRE(c != nullptr);
        REQUIRE_FALSE(c->present);
        REQUIRE(c->freshness == Freshness::expired);
    }

    SECTION("channels expire on their own schedule, not a shared one") {
        // device.battery.temperature max_age is 2 minutes: the same 3-minute-old
        // sample is stale for the battery but still fine for illuminance.
        engine.ingest(reading("device.battery.temperature", 35.0, now - minutes(3)), now);
        engine.ingest(reading("illuminance", 100.0, now - minutes(3)), now);
        auto snap = engine.snapshot(now);
        REQUIRE_FALSE(snap.has("device.battery.temperature"));
        REQUIRE(snap.has("illuminance"));
    }
}

TEST_CASE("Environmental context: fusion propagates uncertainty", "[envcontext]") {
    EnvironmentalContextEngine engine;
    auto now = system_clock::now();

    SECTION("two declared readings fuse by inverse variance") {
        // w1 = 1/1^2 = 1 ; w2 = 1/2^2 = 0.25
        // value = (1*10 + 0.25*20)/1.25 = 12 ; sigma = sqrt(1/1.25) = 0.8944...
        engine.ingest(reading("illuminance", 10.0, now - seconds(30), 1.0, true), now);
        engine.ingest(reading("illuminance", 20.0, now, 2.0, true), now);
        auto snap = engine.snapshot(now);
        const FusedChannel* c = snap.find("illuminance");
        REQUIRE(c != nullptr);
        REQUIRE(c->present);
        REQUIRE(c->contributors == 2);
        REQUIRE(c->value == Approx(12.0));
        REQUIRE(c->uncertainty == Approx(std::sqrt(1.0 / 1.25)).epsilon(1e-9));
        REQUIRE(c->uncertainty_declared);
        REQUIRE(c->uncertainty < 1.0);   // fusing beat the better single input
    }

    SECTION("an undeclared uncertainty falls back to a plain mean and claims no precision") {
        engine.ingest(reading("illuminance", 10.0, now - seconds(30)), now);
        engine.ingest(reading("illuminance", 20.0, now), now);
        auto snap = engine.snapshot(now);
        const FusedChannel* c = snap.find("illuminance");
        REQUIRE(c != nullptr);
        REQUIRE(c->value == Approx(15.0));
        REQUIRE_FALSE(c->uncertainty_declared);
        REQUIRE(c->uncertainty == Approx(0.0));
    }

    SECTION("the newest contributor decides provenance and source") {
        engine.ingest(reading("illuminance", 10.0, now - seconds(30), 0.0, false, 0.9, "", "node:old"), now);
        engine.ingest(reading("illuminance", 20.0, now, 0.0, false, 0.9, "", "node:new"), now);
        auto snap = engine.snapshot(now);
        const FusedChannel* c = snap.find("illuminance");
        REQUIRE(c != nullptr);
        REQUIRE(c->source == "node:new");
        REQUIRE(c->provenance == Provenance::synthetic);
    }
}

TEST_CASE("Environmental context: memory is bounded", "[envcontext]") {
    EnvironmentalContextEngine engine;
    auto now = system_clock::now();
    for (int i = 0; i < 500; ++i) {
        engine.ingest(reading("illuminance", static_cast<double>(i % 100), now - seconds(500 - i), 0.0, false, 0.9), now);
    }
    // max_readings_per_channel defaults to 32.
    REQUIRE(engine.reading_count() <= 32);
    REQUIRE(engine.channel_count() == 1);
    // The retained ones are the NEWEST, so the fused value reflects the tail.
    auto snap = engine.snapshot(now);
    REQUIRE(snap.has("illuminance"));
}

TEST_CASE("Environmental context: positional channels are gated and coarsened", "[envcontext]") {
    auto now = system_clock::now();

    SECTION("disabled by default, even WITH a grant") {
        EnvironmentalContextEngine engine;   // positional_enabled defaults false
        auto r = engine.ingest(reading("position.latitude", 12.971598, now, 0.0, false, 0.9, kGrant), now);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(engine.snapshot(now).has("position.latitude") == false);
    }

    SECTION("enabled but without a grant is still refused") {
        EnvironmentalContextEngine::Config cfg;
        cfg.positional_enabled = true;
        EnvironmentalContextEngine engine(cfg);
        REQUIRE_FALSE(engine.ingest(reading("position.latitude", 12.971598, now), now).accepted);
    }

    SECTION("enabled WITH a grant is accepted, and coarsened on ingest") {
        EnvironmentalContextEngine::Config cfg;
        cfg.positional_enabled = true;
        cfg.coarse_position = true;
        EnvironmentalContextEngine engine(cfg);
        REQUIRE(engine.ingest(reading("position.latitude", 12.971598, now, 0.0, false, 0.9, kGrant), now).accepted);
        auto snap = engine.snapshot(now);
        const FusedChannel* c = snap.find("position.latitude");
        REQUIRE(c != nullptr);
        REQUIRE(c->present);
        // 2 decimal places ~= 1.1 km, so no precise coordinate is retained.
        REQUIRE(c->value == Approx(12.97));
        REQUIRE(c->value != Approx(12.971598));
    }

    SECTION("coarsening can be turned off explicitly, and then the exact value is kept") {
        EnvironmentalContextEngine::Config cfg;
        cfg.positional_enabled = true;
        cfg.coarse_position = false;
        EnvironmentalContextEngine engine(cfg);
        engine.ingest(reading("position.latitude", 12.971598, now, 0.0, false, 0.9, kGrant), now);
        auto snap = engine.snapshot(now);
        const FusedChannel* c = snap.find("position.latitude");
        REQUIRE(c != nullptr);
        REQUIRE(c->value == Approx(12.971598));
    }
}

TEST_CASE("Environmental context: atmospheric channels require authorization", "[envcontext]") {
    EnvironmentalContextEngine engine;
    auto now = system_clock::now();

    SECTION("no authorization context is refused") {
        REQUIRE_FALSE(engine.ingest(reading("atmospheric.temperature", 24.0, now), now).accepted);
    }

    SECTION("an explicit denial is refused") {
        auto r = engine.ingest(
            reading("atmospheric.temperature", 24.0, now, 0.0, false, 0.9,
                    "subject=node:mi-a3-04de;purpose=environment_monitor;decision=denied"),
            now);
        REQUIRE_FALSE(r.accepted);
    }

    SECTION("a grant is accepted and the reading becomes available") {
        REQUIRE(engine.ingest(reading("atmospheric.temperature", 24.0, now, 0.5, true, 0.9, kGrant), now).accepted);
        auto snap = engine.snapshot(now);
        REQUIRE(snap.atmospheric_series_available());
        REQUIRE(snap.atmospheric_temperature().has_value());
        REQUIRE(*snap.atmospheric_temperature() == Approx(24.0));
    }

    SECTION("permission-free channels need no authorization") {
        REQUIRE(engine.ingest(reading("illuminance", 5.0, now), now).accepted);
        REQUIRE(engine.ingest(reading("motion.presence", 1.0, now), now).accepted);
    }
}

TEST_CASE("Environmental context: thresholds must be injected, never invented", "[envcontext]") {
    auto now = system_clock::now();

    SECTION("thermal health is unknown with no injected limits") {
        EnvironmentalContextEngine engine;   // no limits configured
        engine.ingest(reading("device.battery.temperature", 55.0, now), now);
        REQUIRE(engine.thermal_health(now) == EnvironmentalContextEngine::ThermalHealth::unknown);
    }

    SECTION("limits without a stated source are still not enough") {
        EnvironmentalContextEngine::Config cfg;
        cfg.thermal_warn_c = 40.0;
        cfg.thermal_critical_c = 45.0;
        // thermal_limits_source deliberately left empty
        EnvironmentalContextEngine engine(cfg);
        engine.ingest(reading("device.battery.temperature", 46.0, now), now);
        REQUIRE(engine.thermal_health(now) == EnvironmentalContextEngine::ThermalHealth::unknown);
    }

    SECTION("with injected limits and a source, banding works") {
        EnvironmentalContextEngine::Config cfg;
        cfg.thermal_warn_c = 40.0;
        cfg.thermal_critical_c = 45.0;
        cfg.thermal_limits_source = "device thermal trip points (quiet-therm passive)";
        EnvironmentalContextEngine engine(cfg);

        engine.clear();
        engine.ingest(reading("device.battery.temperature", 30.0, now), now);
        REQUIRE(engine.thermal_health(now) == EnvironmentalContextEngine::ThermalHealth::nominal);

        engine.clear();
        engine.ingest(reading("device.battery.temperature", 41.0, now), now);
        REQUIRE(engine.thermal_health(now) == EnvironmentalContextEngine::ThermalHealth::warm);

        engine.clear();
        engine.ingest(reading("device.battery.temperature", 46.0, now), now);
        REQUIRE(engine.thermal_health(now) == EnvironmentalContextEngine::ThermalHealth::hot);
    }

    SECTION("thermal health never reads an atmospheric channel") {
        EnvironmentalContextEngine::Config cfg;
        cfg.thermal_warn_c = 40.0;
        cfg.thermal_critical_c = 45.0;
        cfg.thermal_limits_source = "test fixture";
        EnvironmentalContextEngine engine(cfg);
        // A hot AIR temperature must not be reported as device thermal health.
        engine.ingest(reading("atmospheric.temperature", 50.0, now, 0.5, true, 0.9, kGrant), now);
        REQUIRE(engine.thermal_health(now) == EnvironmentalContextEngine::ThermalHealth::unknown);
    }

    SECTION("daylight is unanswerable without an injected threshold") {
        EnvironmentalContextEngine engine;
        engine.ingest(reading("illuminance", 15000.0, now), now);
        auto snap = engine.snapshot(now);
        REQUIRE_FALSE(snap.daylight().has_value());
    }

    SECTION("daylight answers once a threshold is injected with a source") {
        EnvironmentalContextEngine::Config cfg;
        cfg.daylight_lux_threshold = 1000.0;
        cfg.daylight_threshold_source = "configured at setup";
        EnvironmentalContextEngine engine(cfg);
        engine.ingest(reading("illuminance", 15000.0, now), now);
        REQUIRE(engine.snapshot(now).daylight().value_or(false));
    }
}

TEST_CASE("Environmental context: correlation vocabulary cannot express a prediction",
          "[envcontext]") {
    SECTION("the vocabulary is exactly two non-predictive statuses") {
        const auto& vocab = correlation_status_vocabulary();
        REQUIRE(vocab.size() == 2);
        REQUIRE(std::find(vocab.begin(), vocab.end(), "environmental_correlation") != vocab.end());
        REQUIRE(std::find(vocab.begin(), vocab.end(), "insufficient_evidence") != vocab.end());
    }

    SECTION("no status claims a cause, a forecast or a disaster") {
        // Same discipline DempsterShafer applies by keeping SeismicPrecursor out of
        // its frame of discernment: make the unsupported output unrepresentable.
        const std::vector<std::string> forbidden = {
            "predict", "prediction", "forecast", "will_", "caus", "earthquake",
            "seismic", "calamity", "disaster", "warning", "alert", "flood",
            "species", "animal",
        };
        for (const auto& status : correlation_status_vocabulary()) {
            std::string lower = status;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& bad : forbidden) {
                INFO("status=" << status << " forbidden=" << bad);
                REQUIRE(lower.find(bad) == std::string::npos);
            }
        }
    }
}

TEST_CASE("Environmental context: correlation with a behavioural change", "[envcontext]") {
    EnvironmentalContextEngine engine;
    auto now = system_clock::now();
    engine.ingest(reading("illuminance", 96.0, now, 2.0, true), now);
    auto snap = engine.snapshot(now);

    SECTION("a change inside the lag window correlates, and is stated as correlation") {
        auto change_at = now - seconds(60);
        auto res = correlate_change_with_environment(snap, "illuminance", change_at, seconds(300));
        REQUIRE(res.correlated);
        REQUIRE(res.status == "environmental_correlation");
        REQUIRE(res.channel == "illuminance");
        REQUIRE(res.value == Approx(96.0));
        // Worded as association, not causation or forecast.
        REQUIRE(res.reason.find("within the accepted window") != std::string::npos);
    }

    SECTION("a change outside the lag window is insufficient evidence") {
        auto change_at = now - seconds(3600);
        auto res = correlate_change_with_environment(snap, "illuminance", change_at, seconds(300));
        REQUIRE_FALSE(res.correlated);
        REQUIRE(res.status == "insufficient_evidence");
    }

    SECTION("a channel with no fresh reading is insufficient evidence") {
        auto res = correlate_change_with_environment(snap, "atmospheric.temperature", now, seconds(300));
        REQUIRE_FALSE(res.correlated);
        REQUIRE(res.status == "insufficient_evidence");
    }
}

TEST_CASE("Environmental context: replay is deterministic and provenance survives",
          "[envcontext]") {
    auto now = system_clock::now();

    auto build = [&](EnvironmentalContextEngine& engine) {
        engine.ingest(reading("illuminance", 96.12, now - seconds(120), 3.0, true), now);
        engine.ingest(reading("illuminance", 101.5, now - seconds(60), 3.0, true), now);
        engine.ingest(reading("device.battery.temperature", 33.5, now), now);
        engine.ingest(reading("motion.presence", 1.0, now), now);
    };

    EnvironmentalContextEngine a, b;
    build(a);
    build(b);

    SECTION("the same input sequence produces byte-identical output") {
        REQUIRE(a.snapshot(now).to_json() == b.snapshot(now).to_json());
    }

    SECTION("synthetic provenance is preserved end to end") {
        auto snap = a.snapshot(now);
        const FusedChannel* c = snap.find("illuminance");
        REQUIRE(c != nullptr);
        REQUIRE(c->provenance == Provenance::synthetic);
        REQUIRE(snap.to_json().find("\"provenance\":\"synthetic\"") != std::string::npos);
    }

    SECTION("clear() returns the engine to empty") {
        a.clear();
        REQUIRE(a.reading_count() == 0);
        REQUIRE(a.channel_count() == 0);
    }
}

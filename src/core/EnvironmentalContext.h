#pragma once

// Environmental context engine (HG-EDGE-05) - the Android-to-C++ sensor interface.
//
// WHY THIS EXISTS. The Mi A3 sensor inventory (docs/EDGE_DEVICE_SENSORS_MI_A3.md)
// established that of the four metrics the engine already allowlists in
// EnvironmentalBounds (temperature | humidity | pressure | illuminance) the phone can
// supply exactly ONE - illuminance. Ambient temperature, barometric pressure and
// humidity are absent hardware, while the device exposes 60+ thermal zones whose names
// (battery, bms, skin-therm, xo-therm, ...) actively invite the substitution the
// operator forbade. This module is the infrastructure that makes that substitution
// structurally impossible rather than merely discouraged.
//
// THREE DESIGN RULES, each enforced in code below:
//
//  1. ATMOSPHERIC AND DEVICE THERMAL ARE DIFFERENT CHANNEL NAMESPACES.
//     `atmospheric.temperature` is air; `device.battery.temperature` and
//     `device.soc.temperature` are the handset's own heat. They never share a
//     channel, `is_atmospheric()` and `is_device_thermal()` are disjoint, and the
//     accessors `ContextSnapshot::atmospheric_temperature()` and
//     `battery_temperature()` cannot return each other's data. A plausible-looking
//     battery reading is not "close enough" - it is a different physical quantity.
//
//  2. EVERY READING DECLARES PROVENANCE, UNCERTAINTY, QUALITY AND FRESHNESS.
//     `Provenance` is the existing synthetic/real split from Observation.h, so a
//     replay fixture can never masquerade as a live measurement. Uncertainty is in
//     the channel's own unit and is required to be non-negative; fusion is
//     inverse-variance weighted and propagates it rather than discarding it.
//     Freshness is COMPUTED from (now - observed_at) against a per-channel limit -
//     it is never asserted by the caller.
//
//  3. NOTHING IS INFERRED FROM AN UNSTATED THRESHOLD.
//     Sunlight-or-not and thermal-health banding both need a threshold, and a
//     threshold invented here would be exactly the "arbitrary threshold" this
//     project forbids. So both are INJECTED via Config, each carrying a
//     `*_source` string naming where the number came from; with no configuration
//     the accessors return std::nullopt/unknown rather than a guess. The thermal
//     banding exists to protect the phone, and its limits should come from the
//     device's own thermal trip points, not from this file.
//
// POSITIONAL CHANNELS ARE OFF BY DEFAULT. The Mi A3 has working GNSS, but
// continuous location tracking was explicitly not authorized. `position.*` is
// therefore gated twice: `positional_enabled` (default false) AND a per-reading
// authorization grant. When enabled, positions are coarsened by default so no
// precise coordinate is retained. There is no code path that turns this on by itself.

#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

// ---------------------------------------------------------------------------
// Channel taxonomy
// ---------------------------------------------------------------------------

enum class ChannelClass {
    atmospheric,     // air: temperature / humidity / pressure. NOT the device.
    illuminance,     // ambient light
    device_thermal,  // the handset's own heat (battery, SoC). NEVER atmospheric.
    positional,      // latitude / longitude / altitude - gated + coarsened
    kinematic,       // motion and orientation
    external,        // weather service, terrain/elevation dataset, Home Assistant
};

const char* channel_class_to_string(ChannelClass c);

struct ChannelSpec {
    const char* channel;                    // canonical dotted name
    ChannelClass cls;
    const char* unit;
    double min_value;                       // plausibility floor (sensor-fault guard)
    double max_value;                       // plausibility ceiling
    std::chrono::seconds max_age;           // freshness limit
    bool requires_authorization;            // explicit consent needed per reading
    const char* notes;
};

// Every supported channel. An unknown channel is REFUSED, never guessed.
const std::vector<ChannelSpec>& channel_registry();

// nullptr when the channel is not supported.
const ChannelSpec* channel_spec(const std::string& channel);

bool channel_supported(const std::string& channel);
bool is_atmospheric(const std::string& channel);
bool is_device_thermal(const std::string& channel);

// ---------------------------------------------------------------------------
// Readings
// ---------------------------------------------------------------------------

struct EnvironmentalReading {
    std::string channel;                    // must be in the registry
    std::string source;                     // e.g. "node:mi-a3-04de", "ha:weather.home"
    double value = 0.0;
    std::string unit;                        // must match the spec (documentation)
    std::chrono::system_clock::time_point observed_at{};
    std::chrono::system_clock::time_point received_at{};
    Provenance provenance = Provenance::synthetic;
    double quality = 0.0;                    // [0,1] - 0 means "unstated", not "bad"
    double uncertainty = 0.0;                // >= 0, in the channel's unit
    bool uncertainty_declared = false;       // false => uncertainty is a placeholder
    std::string authorization;               // required for auth-gated channels
    std::string note;                        // bounded, non-sensitive
};

enum class Freshness { unknown, fresh, stale, expired };

const char* freshness_to_string(Freshness f);

// ---------------------------------------------------------------------------
// Fused view
// ---------------------------------------------------------------------------

struct FusedChannel {
    std::string channel;
    ChannelClass cls = ChannelClass::atmospheric;
    bool present = false;
    double value = 0.0;
    double uncertainty = 0.0;                // propagated, same unit
    bool uncertainty_declared = false;
    double quality = 0.0;
    Freshness freshness = Freshness::unknown;
    std::chrono::seconds age{0};
    std::size_t contributors = 0;
    Provenance provenance = Provenance::synthetic;
    std::string source;
};

struct ContextSnapshot {
    std::chrono::system_clock::time_point at{};
    std::vector<FusedChannel> channels;
    // Set only when a daylight threshold was INJECTED into the engine config with a
    // stated source. Left nullopt otherwise, because an unstated threshold is a guess.
    std::optional<bool> daylight_flag;

    const FusedChannel* find(const std::string& channel) const;
    bool has(const std::string& channel) const;

    // The honesty accessors this whole module exists for.
    // True only when at least one *atmospheric* channel is present and fresh.
    bool atmospheric_series_available() const;

    // Air temperature. Returns nullopt when the phone cannot measure it - which is
    // the normal case on the Mi A3 and is a real answer, not a failure. It will
    // NEVER fall back to a device/battery temperature.
    std::optional<double> atmospheric_temperature() const;

    // The handset's own battery temperature, separate by construction.
    std::optional<double> battery_temperature() const;

    // Requires a configured daylight threshold; nullopt when unconfigured.
    std::optional<bool> daylight() const { return daylight_flag; }

    std::string to_json() const;   // bounded, non-sensitive summary
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

class EnvironmentalContextEngine {
public:
    struct Config {
        std::chrono::seconds max_future_skew{120};     // observed_at may not exceed now by more
        std::size_t max_readings_per_channel = 32;     // bounded memory
        std::size_t max_channel_name_chars = 64;
        std::size_t max_source_chars = 96;
        std::size_t max_note_chars = 160;

        // Positional channels: OFF unless an operator turns them on, and even then
        // each reading needs an authorization grant.
        bool positional_enabled = false;
        bool coarse_position = true;                   // round to ~1 km

        // Injected thresholds. Unset => the corresponding accessor refuses to answer.
        std::optional<double> daylight_lux_threshold;
        std::string daylight_threshold_source;
        std::optional<double> thermal_warn_c;
        std::optional<double> thermal_critical_c;
        std::string thermal_limits_source;

        Config() = default;
    };

    struct IngestResult {
        bool accepted = false;
        std::string reason;    // bounded
    };

    enum class ThermalHealth { unknown, nominal, warm, hot };

    static Config defaults() { return Config{}; }
    explicit EnvironmentalContextEngine(Config cfg = defaults()) : cfg_(std::move(cfg)) {}

    // Fail-closed validation + store. Rejections are bounded and reasoned; nothing
    // is stored on rejection.
    IngestResult ingest(const EnvironmentalReading& reading,
                        std::chrono::system_clock::time_point now);

    // Current combined view. Fusion is inverse-variance weighted per channel and
    // prunes anything past its freshness limit.
    ContextSnapshot snapshot(std::chrono::system_clock::time_point now) const;

    // unknown unless BOTH limits were injected with a source.
    ThermalHealth thermal_health(std::chrono::system_clock::time_point now) const;

    // Validate without storing - useful for gating a replay stream.
    IngestResult validate(const EnvironmentalReading& reading,
                         std::chrono::system_clock::time_point now) const;

    std::size_t reading_count() const;
    std::size_t channel_count() const;
    void clear();

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    std::map<std::string, std::vector<EnvironmentalReading>> by_channel_;
};

// ---------------------------------------------------------------------------
// Correlation with behavioural detections
// ---------------------------------------------------------------------------
//
// Environmental change is joined to an activity/audio change as CORRELATION ONLY.
// The output vocabulary is deliberately tiny: "environmental_correlation" or
// "insufficient_evidence". There is no predicate here that can express a disaster,
// a prediction or a species, and a test asserts the vocabulary stays that way -
// the same discipline DempsterShafer applies by keeping SeismicPrecursor out of its
// frame of discernment.

struct EnvironmentalCorrelationResult {
    bool correlated = false;
    std::string status;          // "environmental_correlation" | "insufficient_evidence"
    std::string channel;
    double value = 0.0;
    double uncertainty = 0.0;
    long long lag_seconds = 0;   // signed: reading_time - change_time
    std::size_t change_count = 0;
    std::string reason;
};

// Correlate a behavioural change time against a channel in the snapshot, within
// cfg_max_lag. Reports correlation, never causation and never a forecast.
EnvironmentalCorrelationResult correlate_change_with_environment(
    const ContextSnapshot& snapshot,
    const std::string& channel,
    std::chrono::system_clock::time_point change_at,
    std::chrono::seconds max_lag);

// The full set of status strings this module can emit, so a caller (and a test) can
// assert that no predictive vocabulary exists.
const std::vector<std::string>& correlation_status_vocabulary();

}  // namespace homeguardian

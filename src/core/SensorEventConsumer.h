#pragma once

// SensorEventConsumer: the boundary where another platform's observations become evidence.
//
// WHY: the core engine must consume normalized observations, never raw frames, samples or platform APIs
// (docs/SENSOR_EVENT_CONTRACT.md). Something has to decide whether an incoming JSON event is a valid,
// trustworthy observation before the fusion engine ever sees it, and that decision has to be explicit
// and testable rather than implied by parsing.
//
// WHAT IT VALIDATES
//   Schema and field types.  Source identity against the prefixes this consumer trusts.  Timestamps,
//   including future and out-of-order ones.  Replay: the same observation id twice is refused, not
//   fused twice.  Consent, which stops capture instead of continuing.  Provenance integrity, including
//   a "real" claim that the source itself contradicts.  A producer that reports its source offline,
//   which yields NO evidence rather than a fabricated quiet.
//
// WHAT IT IS NOT: not an authentication layer, not a transport, and not a place where a platform
// adapter's opinion becomes fact. Every refusal has a closed code so a caller can act on it.

#include "core/FusionEngine.h"
#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace homeguardian {

// Closed vocabulary: a caller can switch on this without parsing prose.
enum class IngestCode {
    accepted,
    not_an_event,         // type is not "inference"
    unsupported_schema,   // schema_version this consumer does not know
    malformed,            // a required field is missing or has the wrong JSON type
    source_identity,      // source empty, or not one this consumer is configured to trust
    provenance_invalid,   // provenance is not real or synthetic (an observation is one or the other;
                          // "mixed" is a property the FUSION ENGINE derives from its members)
    provenance_conflict,  // a "real" claim from a source that identifies itself as a fixture
    future_timestamp,     // beyond the clock-skew allowance
    stale_or_replay,      // older than the last accepted observation
    duplicate,            // this observation id was already accepted
    consent_missing,      // consent required and not granted: capture stops
    source_offline,       // the producer reports the source unavailable: no evidence, not quiet
};

const char* ingest_code_to_string(IngestCode code);

struct IngestResult {
    bool accepted = false;
    IngestCode code = IngestCode::malformed;
    std::string detail;
};

class SensorEventConsumer {
public:
    // Declared before Config: a static constant may precede the struct, but Config{} cannot be a default
    // argument inside the class that is still defining Config's own default member initializers.
    static constexpr size_t kDefaultMaxWindow = 64;

    struct Config {
        // Empty means "any non-empty source". Otherwise the source must start with one of these, so a
        // producer cannot claim to be a platform this consumer does not trust.
        std::vector<std::string> trusted_source_prefixes{};
        std::chrono::seconds clock_skew{5};
        bool require_consent = true;
        std::string consent_field = "consent";
        // A substring in `source` that means "this is a fixture". A real provenance claim from such a
        // source is refused rather than believed.
        std::vector<std::string> fixture_source_hints{"fixture", "synthetic", "simulator", "test:"};
            size_t max_window = kDefaultMaxWindow;
    };

    explicit SensorEventConsumer(Config cfg);

    // Validate and, if it passes, normalize into the window. `now` is injected so time is testable.
    IngestResult ingest(const nlohmann::json& event, std::chrono::system_clock::time_point now);

    const std::vector<Observation>& window() const { return window_; }
    // Fuse what was accepted, through the existing engine - the consumer adds no verdict logic of its own.
    FusionResult fuse() const;
    bool source_available() const { return source_available_; }
    size_t accepted() const { return accepted_; }
    size_t rejected() const { return rejected_; }
    bool saw_observation(const std::string& observation_id) const;

private:
    Config cfg_{};
    std::vector<Observation> window_{};
    std::vector<std::string> seen_ids_{};
    bool have_last_timestamp_ = false;
    std::chrono::system_clock::time_point last_timestamp_{};
    bool source_available_ = true;
    size_t accepted_ = 0;
    size_t rejected_ = 0;
};

}  // namespace homeguardian

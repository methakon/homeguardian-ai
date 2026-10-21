#include "core/AudioObservation.h"

#include <iomanip>
#include <sstream>

namespace homeguardian {

AudioObservation observe_audio_window(const int16_t* pcm, size_t frames, uint32_t channels,
                                      uint32_t sample_rate, Provenance provenance,
                                      const std::string& source) {
    AudioObservation observation;
    observation.provenance = provenance;
    observation.source = source;
    observation.sample_rate = sample_rate;

    const AudioFrameFeatures features = extract_audio_features(pcm, frames, channels, sample_rate);
    observation.features = features;
    if (!features.valid) {
        observation.reason = features.reason;
        return observation;
    }

    // The event description is a LEVEL description, not a classification: what was measured, nothing
    // about what made the sound.
    std::ostringstream event;
    event << "sound level " << std::fixed << std::setprecision(3) << features.rms
          << " RMS (peak " << features.peak << ") with zero-crossing rate "
          << features.zero_crossing_rate;
    observation.event = event.str();
    observation.uncertainty =
        "this is a bounded sound measurement: not speech, not a keyword, not a species, not an event "
        "classification, and nothing is inferred about a cause";
    // The measurement's own strength, bounded: how far the window is above the silence floor, and how
    // much structure the zero-crossing rate shows. No model, no learned parameter.
    const double level = features.rms > 1.0 ? 1.0 : (features.rms < 0.0 ? 0.0 : features.rms);
    const double structure = features.zero_crossing_rate > 0.5
                                 ? 1.0
                                 : (features.zero_crossing_rate < 0.0 ? 0.0
                                                                      : features.zero_crossing_rate);
    observation.confidence = 0.5 * level + 0.5 * structure;
    observation.valid = true;
    return observation;
}

nlohmann::json audio_observation_to_json(const AudioObservation& o) {
    return nlohmann::json{
        {"type", "inference"},
        {"schema_version", 1},
        {"source", o.source},
        {"provenance", provenance_to_string(o.provenance)},
        {"status", o.valid ? "detected" : "unavailable"},
        {"confidence", o.confidence},
        {"detector_id", o.detector_id},
        {"model_id", o.model_id},
        {"event", o.event},
        {"uncertainty", o.uncertainty},
        {"features", {{"rms", o.features.rms},
                      {"peak", o.features.peak},
                      {"zero_crossing_rate", o.features.zero_crossing_rate},
                      {"sample_rate", o.features.sample_rate},
                      {"channels", o.features.channels}}},
    };
}

}  // namespace homeguardian

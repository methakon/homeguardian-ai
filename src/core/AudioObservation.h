#pragma once

// AudioObservation: the audio path's half of the platform-neutral sensor event contract.
//
// WHY: the camera side already turns frames into bounded scalars and ONE explainable finding, with
// provenance carried from the caller. The microphone side needs the same shape so the fusion engine can
// consume sound events the way it consumes motion - without the engine knowing whether the samples came
// from ALSA, Android's AudioRecord, a network node or a file.
//
// WHAT IT IS: a bounded measurement (the existing closed-form descriptors: RMS, peak, zero-crossing
// rate), one status, one confidence, and the provenance of the samples. Nothing else.
//
// WHAT IT IS NOT: no speech, no keyword, no species, no identification of any kind, and no disaster
// claim. A sound event is a sound event: the vocabulary is closed in the same way the camera's is. This
// interface deliberately has no member that could hold a recognised word, a species or an event class.
//
// SEPARATION FROM THE CONTROL PLANE: observations are produced locally and stay local. Nothing here
// transmits media, and nothing here is routed through the Oracle control plane - see
// docs/SENSOR_EVENT_CONTRACT.md.

#include "core/BoundedFeatures.h"
#include "core/Observation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian {

// One bounded sound measurement. `provenance` is stated by the caller and never guessed: a fixture is
// synthetic until a real device says otherwise.
struct AudioObservation {
    bool valid = false;
    std::string reason;
    Provenance provenance = Provenance::synthetic;
    std::string detector_id = "audio-activity";
    std::string model_id = "rms-peak-zcr-v1";
    std::string source;                 // e.g. "alsa:default", "android:audiorecord", "node:kitchen"
    uint32_t sample_rate = 48000;
    AudioFrameFeatures features{};      // rms, peak, zero_crossing_rate - the existing extractors
    std::string event;                  // a closed description, e.g. "sound level above the baseline"
    std::string uncertainty;            // what this explicitly does NOT mean
    double confidence = 0.0;            // [0,1], the measurement's own strength
    std::chrono::system_clock::time_point observed_at = std::chrono::system_clock::now();
};

// Any platform that can produce PCM can implement this. The engine depends on the interface, never on
// ALSA, Android or a specific device.
class IAudioObservationSource {
public:
    virtual ~IAudioObservationSource() = default;
    virtual std::string platform() const = 0;                   // "alsa", "android", "node", "file"
    virtual bool open(std::string* error) = 0;
    virtual void close() = 0;
    // Returns false when no window was available (the caller treats that as no evidence, not as quiet).
    virtual bool read_window(std::vector<int16_t>& pcm, size_t* frames, uint32_t* channels,
                             uint32_t* sample_rate) = 0;
};

// Turn one PCM window into a bounded observation. Refuses (valid=false with a reason) rather than
// guessing when the window is unusable.
AudioObservation observe_audio_window(const int16_t* pcm, size_t frames, uint32_t channels,
                                      uint32_t sample_rate, Provenance provenance,
                                      const std::string& source);

// The observation's own JSON, carrying provenance, status and confidence - the same keys the fusion
// engine reads for any other modality.
nlohmann::json audio_observation_to_json(const AudioObservation& observation);

}  // namespace homeguardian

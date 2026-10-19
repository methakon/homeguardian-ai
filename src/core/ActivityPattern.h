#pragma once

// ActivityPattern: the TEMPORAL shape of what the camera saw, as a bounded description.
//
// WHY THIS EXISTS AND WHAT IT IS NOT
//   The change character says what ONE change looked like (compact or spread, unipolar or not). It cannot
//   say whether changes REPEAT, which is the difference between a small body moving back and forth and a
//   one-off event. This adds that dimension and nothing else.
//
//   It is a motion-pattern description. It does not identify anything: not a species, not a size class,
//   not a cause. A repeated small compact travelling change is equally consistent with a rodent, an
//   insect, a curtain in a draught, or a shadow on a moving surface, and the finding says so in its own
//   uncertainty text rather than letting a reader assume the interesting case.
//
// OPT-IN AND ANALYSIS-ONLY
//   `enabled` defaults to false and this analysis is wired into no alert path. It consumes bounded scalars
//   and returns a description; promoting it into the animal-activity hypothesis path would be a separate,
//   reviewed integration, which is exactly how the experimental layers here have been kept honest.

#include <cstddef>
#include <string>
#include <vector>

namespace homeguardian {

// One observation's bounded scalars, in order. Nothing here is a frame, a pixel or an identity.
struct ActivitySample {
    double changed_fraction = 0.0;   // fraction of pixels that changed
    double spread_radius = 0.0;      // RMS spread of the changed mass / frame diagonal
    double region_travel = 0.0;      // centroid displacement since the previous characterised frame
};

enum class ActivityPattern {
    quiet,               // no change above the floor
    isolated_change,     // one change, and no repetition
    repeated_localized,  // repeated small compact changes that travel: something small moving about
    sustained_activity,  // a large change, or change that does not stop
    erratic,             // changes with no consistent amplitude
};

const char* activity_pattern_to_string(ActivityPattern p);

struct ActivityPatternConfig {
    bool enabled = false;               // OPT-IN. Disabled means the analysis says so.
    size_t max_samples = 64;            // bounded: the most recent samples, no more
    // The measured quiet floor. A change below it is NOT automatically "nothing happened": small AND
    // spread is the quiet rule, so a small compact change still counts as an event (see the .cpp).
    double episode_min_fraction = 0.02;
    double small_fraction_max = 0.05;   // "small" change, a small body rather than a person crossing
    double compact_radius_max = 0.18;   // the same compactness bound the character classifier uses
    double travel_min = 0.005;          // the change must actually move
    double amplitude_tolerance = 0.35;  // how close two amplitudes must be to count as a repeat
    size_t min_repeats = 3;             // how many similar episodes make a repetition
    double sustained_fraction = 0.30;   // at or above this the scene is sustaining a large change
};

struct ActivityPatternFinding {
    bool valid = false;
    std::string reason;
    ActivityPattern pattern = ActivityPattern::quiet;
    size_t samples_used = 0;
    size_t episodes = 0;        // changes above the floor
    size_t repeats = 0;         // episodes that were small, compact, travelling and alike
    double mean_amplitude = 0.0;
    double confidence = 0.0;    // bounded [0,1], closed-form, no learned parameter
    std::string description;    // what was measured
    std::string uncertainty;    // what it does NOT mean
};

ActivityPatternFinding characterise_activity_pattern(const std::vector<ActivitySample>& samples,
                                                    const ActivityPatternConfig& config = {});

// ---------------------------------------------------------------------------------------------------
// The AUDIO half of the same research: the temporal shape of what was heard, from the bounded features
// the microphone adapter already produces (RMS and zero-crossing rate). Same discipline as above - a
// closed vocabulary, opt-in, and a description of levels and repetition rather than of what made the
// sound. It cannot name a sound event, a word or a species, and does not try.

struct AudioActivitySample {
    double rms = 0.0;                  // from observe_audio_window()
    double zero_crossing_rate = 0.0;   // ditto; kept so a future pattern can use shape, not only level
};

enum class AudioActivityPattern {
    quiet,            // nothing above the floor in the window
    isolated_event,   // one sound, and no repetition
    repeated_events,  // repeated, similar-level sounds: something recurring
    sustained_level,  // a level that stays up for most of the window
    varying,          // sounds with no consistent level
};

const char* audio_activity_pattern_to_string(AudioActivityPattern p);

struct AudioPatternConfig {
    bool enabled = false;             // OPT-IN, like the motion analysis
    size_t max_samples = 64;          // bounded
    double floor_rms = 0.02;          // above this, something was audible
    double event_min_rms = 0.05;      // an event is clearly above the floor
    // "Alike" means mutually alike, not merely near the median. Two clusters whose levels differ by a
    // factor of fifteen are not a repetition, and the first version of this rule called them one because
    // it only counted events close to the median. The dispersion of the EVENT SET is therefore tested too.
    double similar_ratio = 0.25;      // levels within a quarter of each other are alike
    size_t min_repeats = 3;
    double sustained_occupancy = 0.6; // fraction of the window above the floor that means "sustained"
};

struct AudioPatternFinding {
    bool valid = false;
    std::string reason;
    AudioActivityPattern pattern = AudioActivityPattern::quiet;
    size_t samples_used = 0;
    size_t events = 0;
    size_t repeats = 0;
    double mean_event_rms = 0.0;
    double occupancy = 0.0;           // fraction of samples above the floor
    double confidence = 0.0;          // bounded [0,1]
    std::string description;
    std::string uncertainty;
};

AudioPatternFinding characterise_audio_activity(const std::vector<AudioActivitySample>& samples,
                                               const AudioPatternConfig& config = {});

}  // namespace homeguardian

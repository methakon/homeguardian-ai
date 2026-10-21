#pragma once

// ChangeCharacter: the bounded, explainable decision that separates a LIGHTING change from a LOCALIZED
// change in the same field of view.
//
// WHY THIS EXISTS
// ---------------
// The camera path reduces a frame pair to one scalar (the changed-pixel fraction), and two physically
// different things produce the same scalar: a room light switching on changes most of the plane at
// once, and a body crossing the field changes a compact region. Measured on real captures, ordinary
// ambience produced confirmed sustained increases in three out of three runs, and a synthetic lighting
// transient produced an alert through the optional episode path - so the missing distinction is not a
// threshold, it is a second dimension of evidence.
//
// WHAT IT USES (all closed-form, from extract_region_change_features)
//   * spatial concentration - how much of the change sits in the densest grid cell;
//   * how much of the grid is touched at all;
//   * polarity - a brightness step moves every pixel the same way, a body appearing here and vacating
//     there does not;
//   * where the changed mass is, so a caller can compare two frames and see travel.
//
// WHAT IT DOES NOT DO
//   * It does not identify anything: no person, animal, species, face or event claim. Its vocabulary is
//     three descriptions of a luminance change.
//   * It does not decide alone. Every opt-in gate that consumes it defaults to OFF, so this component
//     existing changes no behaviour in the release.
//   * It cannot separate a LOCALIZED shadow from a LOCALIZED object, and the tests say so: a moving
//     shadow has both polarities and a compact region, exactly like a moving object, because both are
//     a moving luminance boundary. That limitation is physical, not a tuning problem.

#include "core/BoundedFeatures.h"

#include <string>

namespace homeguardian {

enum class ChangeCharacter {
    unknown,               // not enough information (no geometry, no previous frame, refused input)
    quiet,                 // no pixels changed meaningfully
    global_illumination,   // the change spans the frame and moves one way: a light, an exposure shift
    localized_unipolar,    // a compact change that only brightens or only darkens: an object appearing
                           // or a light source switching on IN the scene - not motion
    localized_motion,      // a compact change with both polarities: something moved through the field
    diffuse_motion,        // change spread widely with both polarities: large or close movement, rain, foliage
    illumination_settling  // a change within a few observations of a global illumination change, with no
                           // region travel: the camera's own response decaying, and what the brightness
                           // step measured on real hardware leaves behind (3-4 observations per step)
};

inline const char* change_character_to_string(ChangeCharacter c) {
    switch (c) {
        case ChangeCharacter::unknown:             return "unknown";
        case ChangeCharacter::quiet:               return "quiet";
        case ChangeCharacter::global_illumination: return "global_illumination";
        case ChangeCharacter::localized_unipolar:  return "localized_unipolar";
        case ChangeCharacter::localized_motion:    return "localized_motion";
        case ChangeCharacter::diffuse_motion:      return "diffuse_motion";
        case ChangeCharacter::illumination_settling: return "illumination_settling";
    }
    return "unknown";
}

// True for the two characters that describe MOVEMENT of something within the field of view. A caller
// that wants "motion, not merely change" tests this and nothing else.
inline bool is_localized_or_diffuse_motion(ChangeCharacter c) {
    return c == ChangeCharacter::localized_motion || c == ChangeCharacter::diffuse_motion;
}

// The classifier's own thresholds. These are EXPERIMENTAL design parameters of a new component, not
// changes to any existing safety threshold: the detector's confirm ratio, the rule's 0.5 gate and the
// engine's 0.6 agreement threshold are all untouched and are not read here. Each value below is
// justified in docs/CHANGE_CHARACTER_SEPARATION.md against synthetic frames whose pixel-level truth is
// known, and every one of them is a starting point for real-camera validation rather than a result.
struct ChangeCharacterPolicy {
    // Below this share of changed pixels, call it quiet. CALIBRATED FROM A REAL RUN, not chosen: the
    // sensor's idle band measures 0.0094-0.0106 of pixels crossing the delta threshold (p10-median-p90,
    // 102 of 120 observations, mean_delta 0.0065), and the still-scene ceiling is 0.0103. A floor of
    // 0.002 labelled that idle noise as diffuse motion in 117 of 120 frames - the classifier was
    // reading the sensor, not the scene. 0.02 sits clear above the idle band and far below real
    // movement (the same run reached 0.3522). This is a descriptor threshold belonging to a new
    // component; it is not a change to any detector or rule threshold.
    // ...and the change must also be SPREAD to count as noise: a small but COMPACT change is a small
    // object, not the sensor. That joint rule is what lets a 0.7%-of-pixels moving body be classified
    // while a 1% scattered idle band stays quiet.
    double quiet_fraction = 0.02;
    double uniform_polarity = 0.10;     // below this bipolar balance, the change is one-directional
    double compact_radius = 0.18;       // RMS spread / frame diagonal below this = a compact change
    // At or above this correlation between the delta and the scene's own brightness, the change is
    // explained as a scaling of the scene - a gain, brightness or exposure change - whatever its spatial
    // shape. Real captures show the camera's automatic gain producing compact, structured changes that
    // no spatial statistic can separate from motion.
    double gain_correlation_illumination = 0.6;
    // A change inside this many observations of a global illumination change, with a changed-mass
    // centroid that has not travelled, is the illumination event's own settling tail. Measured on real
    // frames: every motion-labelled change in a scene where the only deliberate change was illumination
    // sat 5-20 observations after a step, in tails 3 observations long. The window is the measured tail
    // length (16 observations = 4 s at the 5-frame cadence), and its cost is stated: in-place movement
    // within four seconds of a light change is attributed to the light, travelling movement is not.
    size_t illumination_tail_observations = 16;
    double settling_travel_epsilon = 0.02;

    static ChangeCharacterPolicy defaults() { return ChangeCharacterPolicy{}; }
};

// Classify one frame pair. Returns `unknown` when the descriptor itself is invalid, and never throws.
ChangeCharacter classify_change(const RegionChangeFeatures& f,
                                const ChangeCharacterPolicy& policy = ChangeCharacterPolicy{});

// A one-line, non-sensitive explanation of the verdict, for logs and observation features.
std::string change_character_reason(const RegionChangeFeatures& f, ChangeCharacter verdict);

}  // namespace homeguardian

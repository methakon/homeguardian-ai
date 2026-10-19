#pragma once

// Bounded, deterministic feature extraction from PERMITTED media buffers.
//
// WHY: the animal/natural-hazard precursor work (see the
// `animal-calamity-precursor-detection` skill) needs low-level descriptors
// before any interpretation. This file provides the descriptors and NOTHING
// ELSE - it makes no species, speech, face, fall or disaster claim.
//
// WHAT THIS IS NOT:
//   * NOT speech recognition, keyword spotting or transcription.
//   * NOT species classification. Audio energy is energy; motion is motion.
//   * NOT a model. There is no ML runtime here, no downloaded weights, and no
//     learned parameter: every value below is a closed-form function of the
//     input samples, so it is reproducible and unit-testable on the host.
//
// BOUNDS: the extractors are the innermost loop of a phone/hub pipeline, so they
// take a raw pointer + count (no allocation), REFUSE anything larger than the
// documented maximum instead of allocating for it, reject anything smaller than
// a documented minimum, and report WHY in `reason` rather than throwing.
//
// PRIVACY: features are scalars derived in place. No buffer is copied, stored,
// queued or transmitted by this component.

#include <cstddef>
#include <cstdint>
#include <string>

namespace homeguardian {

// Hard limits. A caller that exceeds them is misconfigured or hostile; refuse.
struct FeatureBounds {
    static constexpr size_t kMaxAudioFrames = 48000;   // 3 s at 16 kHz, per call
    static constexpr size_t kMinAudioFrames = 8;       // below this, statistics are noise
    static constexpr uint32_t kMaxChannels = 2;
    static constexpr uint32_t kMinSampleRate = 8000;
    static constexpr uint32_t kMaxSampleRate = 48000;
    static constexpr size_t kMaxPixels = 640u * 480u;  // 307200 (VGA luma plane)
    static constexpr size_t kMinPixels = 16;
    static constexpr uint8_t kDefaultPixelDelta = 16;  // per-pixel |delta| to count as changed
    static constexpr size_t kMaxVectorDim = 2048;      // feature/embedding vector bound
    static constexpr size_t kMaxRegionCells = 64;      // grid bound for region change descriptors
};

// Loudness/shape descriptors of one PCM window. `rms` and `peak` are normalised
// against int16 full scale; `zero_crossing_rate` is sign changes per sample.
struct AudioFrameFeatures {
    bool valid = false;
    std::string reason;
    uint32_t sample_frames = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    double rms = 0.0;
    double peak = 0.0;
    double zero_crossing_rate = 0.0;
};

// Change descriptors between two luma planes of identical geometry.
struct MotionFrameFeatures {
    bool valid = false;
    std::string reason;
    uint32_t pixels = 0;
    uint32_t changed_pixels = 0;
    double changed_fraction = 0.0;  // changed_pixels / pixels
    double mean_delta = 0.0;        // mean |delta| / 255
};

// Change descriptors that keep WHERE the change happened, not only how much. Two frames can differ by
// the same fraction of pixels and mean nothing alike: a room light switching on changes most of the
// plane and spreads over every cell, while a body crossing the field changes a compact region. The
// descriptors below are the bounded, allocation-free form of that distinction - still closed-form
// arithmetic over the permitted planes, still no model, still no interpretation.
struct RegionChangeFeatures {
    bool valid = false;
    std::string reason;
    uint32_t pixels = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t changed_pixels = 0;
    double changed_fraction = 0.0;
    // Spatial concentration. The plane is divided into a fixed grid; `peak_cell_share` is the share of
    // changed pixels inside the single densest cell (so a compact change approaches 1.0 and a uniform
    // one approaches 1/cells), and `occupied_cell_fraction` is how much of the grid is touched at all.
    double peak_cell_share = 0.0;
    double occupied_cell_fraction = 0.0;
    // Polarity of the change. A brightness step moves every pixel the same way (unipolar); an object
    // appearing in one place and vacating another produces both polarities. `bipolar_balance` is
    // 2*min(brighter, darker)/(brighter + darker): 0.0 unipolar, 1.0 balanced.
    double brighter_share = 0.0;
    double darker_share = 0.0;
    double bipolar_balance = 0.0;
    // Where the changed mass sits, normalised to [0,1]. Comparing two consecutive frames gives the
    // direction of travel; a static global change has no travel.
    double centroid_x = 0.0;
    double centroid_y = 0.0;
    bool centroid_valid = false;
    // How far the changed mass spreads from its own centre, as an RMS radius normalised by the frame
    // diagonal. This is the scale-free form of "was the change compact or all over the frame", and it
    // is what the classifier keys on: a body is a small radius, a light switch is a large one, and the
    // ratio does not change when the object or the frame is a different size.
    double spread_radius = 0.0;
};

// Extract the region-aware change descriptors between two luma planes of identical geometry.
// `grid_x * grid_y` is the cell count and is bounded to keep the work constant; an unsupported grid,
// a geometry that does not multiply to `pixels`, or an out-of-range plane size is refused with a
// reason rather than sampled.
RegionChangeFeatures extract_region_change_features(
    const uint8_t* previous_luma, const uint8_t* current_luma, size_t pixels, uint32_t width,
    uint32_t height, uint8_t delta_threshold = FeatureBounds::kDefaultPixelDelta, uint32_t grid_x = 8,
    uint32_t grid_y = 6);

// Extract from interleaved signed 16-bit PCM. Never reads past `frames*channels`
// samples. Returns valid=false with a reason for: null pointer, out-of-range
// count, unsupported channel count, unsupported sample rate.
AudioFrameFeatures extract_audio_features(const int16_t* pcm,
                                          size_t frames,
                                          uint32_t channels,
                                          uint32_t sample_rate);

// Extract change descriptors between consecutive luma (Y) planes.
// `delta_threshold` is the per-pixel absolute difference counted as "changed".
MotionFrameFeatures extract_motion_features(const uint8_t* previous_luma,
                                            const uint8_t* current_luma,
                                            size_t pixels,
                                            uint8_t delta_threshold =
                                                FeatureBounds::kDefaultPixelDelta);

// NV21 helpers. The Y plane is the first width*height bytes; chroma follows.
// These only VALIDATE geometry and expose the plane - no colour conversion, no
// decode, no scaling, no interpretation.
bool nv21_geometry_ok(size_t width, size_t height, size_t buffer_bytes);
const uint8_t* nv21_y_plane(const uint8_t* buffer);

// ---- bounded statistical helpers (deterministic, shared by the detectors) ----

// Mean of `n` doubles; 0.0 when n == 0.
double bounded_mean(const double* values, size_t n);

// Population standard deviation; 0.0 when n < 2.
double bounded_stddev(const double* values, size_t n);

// Cosine similarity of two `n`-length vectors, in [-1,1]. Returns 0.0 (a
// defined, non-throwing value) when n is 0, when n exceeds
// FeatureBounds::kMaxVectorDim, or when either vector has zero norm.
double cosine_similarity(const double* a, const double* b, size_t n);

// 1 - cosine_similarity, i.e. a distance in [0,2] where 0 means identical
// direction. Direction is what matters for a feature/embedding regime shift;
// magnitude is discarded on purpose.
double cosine_distance(const double* a, const double* b, size_t n);

// True when every element of the `n`-vector is finite.
bool vector_is_finite(const double* a, size_t n);

}  // namespace homeguardian

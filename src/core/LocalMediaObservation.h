#pragma once

// LocalMediaObservation: the ONLY step where raw media becomes something the engine can see.
//
// WHY IT EXISTS AS ITS OWN COMPONENT
//   Media stays on the machine that acquired it, and the engine consumes bounded scalars. That boundary is
//   easy to state and easy to violate by accident, so it lives in one small function with one job: reduce a
//   frame PAIR to a bounded motion measurement. It takes frames, returns numbers, and has no path by which a
//   frame - or any part of one - can continue onward.
//
// WHAT IT IS NOT
//   Not a detector: it measures change, and the existing ActivityChangeDetector and fusion rule decide what
//   that means. Not a codec: the frames it accepts are single-plane luma, which is what the local acquisition
//   path produces after decoding. Not a camera: it works identically for the synthetic fixture used in
//   rehearsal, which is why the whole chain can be exercised without the V380 being reachable.

#include "core/BoundedFeatures.h"
#include "core/BoundedMediaBuffer.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace homeguardian {

struct LocalMediaObservation {
    bool valid = false;
    std::string reason;
    double changed_fraction = 0.0;    // fraction of pixels that changed between the pair
    double mean_delta = 0.0;          // mean absolute luma delta over those pixels
    uint64_t frame_id = 0;            // which frame the measurement belongs to
    std::chrono::system_clock::time_point observed_at{};
};

// Reduce a consecutive frame pair to bounded scalars. Refuses (valid=false with a reason) when the frames do
// not fit the declared geometry, when a frame is empty, or when the pair is not consecutive - rather than
// measuring a pair that is not what the caller thinks it is.
LocalMediaObservation derive_motion_observation(const MediaFrame& previous, const MediaFrame& current,
                                               uint32_t width, uint32_t height,
                                               uint8_t delta_threshold =
                                                   FeatureBounds::kDefaultPixelDelta);

}  // namespace homeguardian

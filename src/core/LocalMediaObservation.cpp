#include "core/LocalMediaObservation.h"

namespace homeguardian {

LocalMediaObservation derive_motion_observation(const MediaFrame& previous, const MediaFrame& current,
                                               uint32_t width, uint32_t height,
                                               uint8_t delta_threshold) {
    LocalMediaObservation out;
    out.frame_id = current.frame_id;
    out.observed_at = current.captured_at;

    if (width == 0 || height == 0) {
        out.reason = "no frame geometry: the caller must declare the luma plane's dimensions";
        return out;
    }
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixels < FeatureBounds::kMinPixels || pixels > FeatureBounds::kMaxPixels) {
        out.reason = "geometry outside the accepted bounds (" + std::to_string(pixels) + " pixels)";
        return out;
    }
    if (previous.bytes.size() < pixels || current.bytes.size() < pixels) {
        out.reason = "a frame is shorter than the declared geometry: refusing rather than measuring a partial "
                     "frame";
        return out;
    }
    if (current.frame_id <= previous.frame_id) {
        out.reason = "the pair is not consecutive (the second frame is not newer)";
        return out;
    }

    const MotionFrameFeatures features =
        extract_motion_features(previous.bytes.data(), current.bytes.data(), pixels, delta_threshold);
    if (!features.valid) {
        out.reason = features.reason.empty() ? "the motion descriptor refused the pair" : features.reason;
        return out;
    }
    out.valid = true;
    out.changed_fraction = features.changed_fraction;
    out.mean_delta = features.mean_delta;
    return out;
}

}  // namespace homeguardian

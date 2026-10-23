#pragma once

// Preview-geometry policy shared by the Android camera backends and the host test suite.
//
// WHY THIS EXISTS - the measured defect:
//   The operator's log showed `frames=518/518` authorized, `frame_bytes=322237440` (622,080 bytes
//   per frame = 720x576 NV21, exactly), and yet `frames_processed=0 obs_emitted=0`. The processing
//   block in the JNI frame callback is gated by
//       pixels >= FeatureBounds::kMinPixels && pixels <= FeatureBounds::kMaxPixels
//   with kMaxPixels = 640x480 = 307,200, and `++frames_processed` lives INSIDE that block. The
//   platform negotiated 720x576 = 414,720 pixels, so every frame was authorized, accounted and then
//   skipped before the counter ever moved. Acquisition was real; processing never ran.
//
// THE RULE, and why the bounds are NOT relaxed: the hard limits exist to cap CPU and allocation on a
// 1 GB device. Loosening kMaxPixels would silently raise that cap for every configuration. Instead the
// camera is asked for a preview size that is INSIDE the bounds in the first place, chosen from the
// sizes the platform actually supports. A platform that offers nothing inside the bounds is reported,
// not accommodated - and a HAL that ignores the request and delivers a larger frame is REFUSED, loudly
// and rate-limited, rather than silently downsampled (downsampling would change the motion descriptor's
// meaning, which is a semantic change hidden behind a size fix).
//
// Pure, allocation-free and dependency-free (std::vector/std::optional only), so it compiles unchanged
// for the APK and for the host tests.

#include <cstddef>
#include <optional>
#include <vector>

namespace homeguardian {

struct PreviewSize {
    int width = 0;
    int height = 0;
    size_t pixels() const { return static_cast<size_t>(width) * static_cast<size_t>(height); }
};

// The single predicate the frame path uses: is this geometry inside the feature bounds? It answers
// with the SAME limits FeatureBounds declares - there is no second definition to drift.
bool geometry_within_feature_bounds(int width, int height);

// Which bound, if any, a geometry violates. `Ok` means processable.
enum class GeometryVerdict {
    Ok,
    NonPositive,
    BelowMinPixels,
    AboveMaxPixels
};
GeometryVerdict classify_geometry(int width, int height);

// The closest SUPPORTED size to `target` that is inside the feature bounds.
// Deterministic: in-bounds only; then smallest |width-target.width| + |height-target.height|; ties
// broken by smaller area, then by smaller width. Returns nullopt when no supported size is
// processable - the caller must then refuse to start capture rather than negotiate a geometry it
// cannot process.
std::optional<PreviewSize> choose_processable_preview_size(const std::vector<PreviewSize>& supported,
                                                           PreviewSize target);

} // namespace homeguardian

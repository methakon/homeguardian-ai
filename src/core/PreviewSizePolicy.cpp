#include "core/PreviewSizePolicy.h"

#include "core/BoundedFeatures.h"

#include <cstdlib>

namespace homeguardian {

bool geometry_within_feature_bounds(int width, int height) {
    return classify_geometry(width, height) == GeometryVerdict::Ok;
}

GeometryVerdict classify_geometry(int width, int height) {
    if (width <= 0 || height <= 0) return GeometryVerdict::NonPositive;
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixels < FeatureBounds::kMinPixels) return GeometryVerdict::BelowMinPixels;
    if (pixels > FeatureBounds::kMaxPixels) return GeometryVerdict::AboveMaxPixels;
    return GeometryVerdict::Ok;
}

std::optional<PreviewSize> choose_processable_preview_size(const std::vector<PreviewSize>& supported,
                                                           PreviewSize target) {
    std::optional<PreviewSize> best;
    long best_distance = 0;
    size_t best_pixels = 0;

    for (const auto& candidate : supported) {
        if (!geometry_within_feature_bounds(candidate.width, candidate.height)) continue;
        const long dx = std::labs(static_cast<long>(candidate.width) - target.width);
        const long dy = std::labs(static_cast<long>(candidate.height) - target.height);
        const long distance = dx + dy;
        const size_t pixels = candidate.pixels();

        const bool better =
            !best.has_value() ||
            distance < best_distance ||
            (distance == best_distance &&
             (pixels < best_pixels || (pixels == best_pixels && candidate.width < best->width)));
        if (better) {
            best = candidate;
            best_distance = distance;
            best_pixels = pixels;
        }
    }
    return best;
}

} // namespace homeguardian

// Preview-geometry policy tests - the fix for "518 authorized frames, 0 processed".
//
// The measured device evidence is encoded here as a regression: a 720x576 NV21 frame (414,720 pixels,
// 622,080 bytes) is AUTHORIZED but NOT processable, because FeatureBounds::kMaxPixels is 640x480
// (307,200). The fix selects a supported preview size inside the bounds instead of relaxing them, and
// these tests pin both halves: the bound itself, and the policy that respects it.

#include "catch2/catch.hpp"

#include "core/BoundedFeatures.h"
#include "core/PreviewSizePolicy.h"

#include <string>
#include <vector>

using namespace homeguardian;

namespace {
std::vector<PreviewSize> sizes(std::initializer_list<PreviewSize> list) {
    return std::vector<PreviewSize>(list);
}
bool same(const PreviewSize& a, const PreviewSize& b) {
    return a.width == b.width && a.height == b.height;
}
} // namespace

TEST_CASE("preview size: the feature bound is exactly 640x480 and the device geometry violates it",
          "[previewsize]") {
    REQUIRE(FeatureBounds::kMaxPixels == 640u * 480u);
    REQUIRE(FeatureBounds::kMinPixels == 16u);

    // The measured device geometry: 518 authorized frames, 622,080 bytes each = 720x576 NV21.
    const size_t measured_bytes_per_frame = 322237440u / 518u;
    REQUIRE(measured_bytes_per_frame == 720u * 576u * 3u / 2u);   // 622,080 - the accounting is exact
    REQUIRE(classify_geometry(720, 576) == GeometryVerdict::AboveMaxPixels);
    REQUIRE_FALSE(geometry_within_feature_bounds(720, 576));

    // The bound is exact, not approximate.
    REQUIRE(geometry_within_feature_bounds(640, 480));
    REQUIRE(classify_geometry(640, 481) == GeometryVerdict::AboveMaxPixels);
    REQUIRE(classify_geometry(320, 240) == GeometryVerdict::Ok);
    REQUIRE(classify_geometry(16, 1) == GeometryVerdict::Ok);            // == kMinPixels
    REQUIRE(classify_geometry(8, 2) == GeometryVerdict::Ok);             // 16
    REQUIRE(classify_geometry(4, 4) == GeometryVerdict::Ok);             // 16
    REQUIRE(classify_geometry(2, 4) == GeometryVerdict::BelowMinPixels); // 8
    REQUIRE(classify_geometry(0, 240) == GeometryVerdict::NonPositive);
    REQUIRE(classify_geometry(-4, 240) == GeometryVerdict::NonPositive);
    REQUIRE_FALSE(geometry_within_feature_bounds(0, 0));
}

TEST_CASE("preview size: the policy picks the closest SUPPORTED size inside the bounds",
          "[previewsize]") {
    const auto chosen = choose_processable_preview_size(
        sizes({{320, 240}, {640, 480}, {720, 576}}), PreviewSize{320, 240});
    REQUIRE(chosen.has_value());
    REQUIRE(same(*chosen, PreviewSize{320, 240}));

    const auto larger = choose_processable_preview_size(
        sizes({{176, 144}, {320, 240}, {640, 480}, {1280, 720}}), PreviewSize{640, 480});
    REQUIRE(larger.has_value());
    REQUIRE(same(*larger, PreviewSize{640, 480}));

    const auto nearest = choose_processable_preview_size(
        sizes({{320, 240}, {640, 480}}), PreviewSize{854, 480});
    REQUIRE(nearest.has_value());
    REQUIRE(same(*nearest, PreviewSize{640, 480}));
}

TEST_CASE("preview size: a platform offering nothing inside the bounds yields NO choice",
          "[previewsize]") {
    // The device's exact failure mode if its smallest supported size were 720x576: the policy must
    // refuse to negotiate a geometry it cannot process, and the caller must refuse to start - never
    // silently accept the frame.
    REQUIRE_FALSE(choose_processable_preview_size(sizes({{720, 576}, {1280, 720}}),
                                                  PreviewSize{320, 240}).has_value());
    REQUIRE_FALSE(choose_processable_preview_size({}, PreviewSize{320, 240}).has_value());
    REQUIRE_FALSE(choose_processable_preview_size(sizes({{2, 4}, {1, 1}}),
                                                  PreviewSize{320, 240}).has_value());
}

TEST_CASE("preview size: ties are broken deterministically (smaller area, then smaller width)",
          "[previewsize]") {
    // Both are 40 pixels away from the target on the L1 metric; the smaller area must win, in list
    // order or reversed - the result may not depend on how the platform listed its sizes.
    const auto a = choose_processable_preview_size(sizes({{320, 240}, {400, 240}}), PreviewSize{360, 240});
    const auto b = choose_processable_preview_size(sizes({{400, 240}, {320, 240}}), PreviewSize{360, 240});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(same(*a, PreviewSize{320, 240}));
    REQUIRE(same(*b, PreviewSize{320, 240}));
}

TEST_CASE("preview size: whatever the policy returns is ALWAYS processable", "[previewsize]") {
    const PreviewSize targets[] = {{320, 240}, {640, 480}, {854, 480}, {1280, 720}, {176, 144}};
    const std::vector<std::vector<PreviewSize>> lists = {
        {{320, 240}, {640, 480}},
        {{176, 144}, {352, 288}, {640, 480}, {720, 576}, {1280, 720}},
        {{640, 480}},
        {{1280, 720}, {1920, 1080}},
    };
    for (const auto& list : lists) {
        for (const auto& target : targets) {
            const auto chosen = choose_processable_preview_size(list, target);
            if (chosen.has_value()) {
                REQUIRE(geometry_within_feature_bounds(chosen->width, chosen->height));
            }
        }
    }
}

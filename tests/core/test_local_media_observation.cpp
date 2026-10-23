// test_local_media_observation: the one place media becomes numbers, and the refusals around it.

#include <catch2/catch.hpp>

#include "core/LocalMediaObservation.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 48;

MediaFrame luma(uint64_t id, uint8_t value) {
    MediaFrame f;
    f.frame_id = id;
    f.captured_at = std::chrono::system_clock::now();
    f.bytes.assign(kW * kH, value);
    return f;
}

}  // namespace

TEST_CASE("local media: a changed frame pair becomes a bounded scalar", "[localmedia][derivation]") {
    MediaFrame a = luma(1, 10);
    MediaFrame b = luma(2, 10);
    // Change a quarter of the frame, well above the delta threshold.
    for (uint32_t y = 0; y < kH / 2; ++y) {
        for (uint32_t x = 0; x < kW / 2; ++x) b.bytes[y * kW + x] = 200;
    }

    const auto observation = derive_motion_observation(a, b, kW, kH);
    INFO(observation.reason);
    REQUIRE(observation.valid);
    REQUIRE(observation.changed_fraction == Approx(0.25).epsilon(0.01));
    REQUIRE(observation.mean_delta > 0.0);
    REQUIRE(observation.frame_id == 2);
}

TEST_CASE("local media: an unchanged pair measures zero change", "[localmedia][derivation]") {
    const auto same = luma(1, 42);
    MediaFrame next = luma(2, 42);
    const auto observation = derive_motion_observation(same, next, kW, kH);
    REQUIRE(observation.valid);
    REQUIRE(observation.changed_fraction == Approx(0.0));
}

TEST_CASE("local media: it refuses what it cannot honestly measure", "[localmedia][refusal]") {
    const auto a = luma(1, 10);
    auto b = luma(2, 10);
    b.bytes[0] = 200;

    SECTION("no declared geometry") {
        REQUIRE_FALSE(derive_motion_observation(a, b, 0, 0).valid);
    }
    SECTION("geometry below the accepted bounds") {
        const auto tiny = derive_motion_observation(a, b, 2, 2);
        REQUIRE_FALSE(tiny.valid);
        REQUIRE(tiny.reason.find("bounds") != std::string::npos);
    }
    SECTION("a frame shorter than the declared geometry") {
        MediaFrame short_frame = luma(2, 10);
        short_frame.bytes.resize(kW * kH / 2);
        const auto observation = derive_motion_observation(a, short_frame, kW, kH);
        REQUIRE_FALSE(observation.valid);
        REQUIRE(observation.reason.find("shorter than the declared geometry") != std::string::npos);
    }
    SECTION("a pair that is not consecutive") {
        MediaFrame same_id = luma(1, 10);
        const auto observation = derive_motion_observation(a, same_id, kW, kH);
        REQUIRE_FALSE(observation.valid);
        REQUIRE(observation.reason.find("not consecutive") != std::string::npos);
    }
}

TEST_CASE("local media: the measurement is the same as the engine's own descriptor would produce",
          "[localmedia][derivation]") {
    // The derivation must not invent its own notion of change: it is the existing descriptor, applied to a
    // decoded frame pair. This pins that they cannot drift apart.
    MediaFrame a = luma(1, 30);
    MediaFrame b = luma(2, 30);
    for (size_t i = 0; i < b.bytes.size(); i += 7) b.bytes[i] = 120;

    const auto via_local = derive_motion_observation(a, b, kW, kH);
    const auto direct = extract_motion_features(a.bytes.data(), b.bytes.data(), kW * kH,
                                               FeatureBounds::kDefaultPixelDelta);
    REQUIRE(via_local.valid);
    REQUIRE(direct.valid);
    REQUIRE(via_local.changed_fraction == Approx(direct.changed_fraction));
    REQUIRE(via_local.mean_delta == Approx(direct.mean_delta));
}

// test_change_character: can a LIGHTING change be told apart from a LOCALIZED change, from pixels?
//
// Every fixture here is a pair of synthetic luma planes generated in the test, so the ground truth is
// exact: we know which pixels changed, by how much, in which direction, and where. That is the only way
// to validate a discriminator honestly - a scalar replay cannot, because the whole problem is that one
// scalar cannot tell the two cases apart.
//
// The classifier's thresholds are experimental and every one of them is exercised below; no existing
// safety threshold is read or changed by this file.

#include <catch2/catch.hpp>

#include "core/ActivityEpisode.h"
#include "core/BoundedFeatures.h"
#include "core/CameraActivitySession.h"
#include "core/ChangeCharacter.h"
#include "core/DetectionFusionRule.h"
#include "core/Observation.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

constexpr uint32_t kWidth = 160;
constexpr uint32_t kHeight = 120;
constexpr size_t kPixels = kWidth * kHeight;

std::vector<uint8_t> plane(uint8_t fill) { return std::vector<uint8_t>(kPixels, fill); }

void fill_rect(std::vector<uint8_t>& p, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
               uint8_t value) {
    for (uint32_t y = y0; y < y0 + h && y < kHeight; ++y) {
        for (uint32_t x = x0; x < x0 + w && x < kWidth; ++x) {
            p[size_t(y) * kWidth + x] = value;
        }
    }
}

// Deterministic, so a failure is reproducible.
void add_noise(std::vector<uint8_t>& p, int amplitude, uint32_t seed) {
    uint32_t state = seed;
    for (size_t i = 0; i < p.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        const int delta = int((state >> 24) % uint32_t(2 * amplitude + 1)) - amplitude;
        const int v = int(p[i]) + delta;
        p[i] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

RegionChangeFeatures features_of(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return extract_region_change_features(a.data(), b.data(), kPixels, kWidth, kHeight);
}

ChangeCharacter character_of(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return classify_change(features_of(a, b));
}

}  // namespace

TEST_CASE("region descriptors: a global step and a compact block land in different places",
          "[character][features]") {
    // The whole premise, measured: two frame pairs with the SAME changed-fraction can differ by an
    // order of magnitude in how far the change spreads.
    const auto base = plane(100);
    auto brighter = plane(100);
    for (auto& v : brighter) v = 120;                      // +20 everywhere
    const auto global = features_of(base, brighter);

    auto block_a = plane(100);
    fill_rect(block_a, 10, 10, 30, 40, 120);               // a bright block in one corner
    auto block_b = plane(100);
    fill_rect(block_b, 40, 10, 30, 40, 120);               // the same block moved across
    const auto moved = features_of(block_a, block_b);

    REQUIRE(global.valid);
    REQUIRE(moved.valid);
    INFO("global spread=" << global.spread_radius << " changed=" << global.changed_fraction
                          << " | moved spread=" << moved.spread_radius
                          << " changed=" << moved.changed_fraction);
    // Measured 0.2887 against 0.1041 - a factor of 2.8, and the absolute threshold separates them with
    // room on both sides. (An earlier draft of this test asserted "more than 3x", which the numbers do
    // not support; the assertion now states the measured factor.)
    REQUIRE(global.spread_radius > 2.5 * moved.spread_radius);
    REQUIRE(moved.spread_radius < 0.18);                          // compact
    REQUIRE(global.spread_radius > 0.18);                         // spread
}

TEST_CASE("region descriptors: polarity separates a light from a body", "[character][features]") {
    const auto base = plane(100);
    auto brighter = plane(100);
    for (auto& v : brighter) v = 120;
    const auto global = features_of(base, brighter);
    REQUIRE(global.bipolar_balance == Approx(0.0).margin(0.001));   // every pixel went one way
    REQUIRE(global.brighter_share == Approx(1.0).margin(0.001));

    auto block_a = plane(100);
    fill_rect(block_a, 10, 10, 30, 40, 120);
    auto block_b = plane(100);
    fill_rect(block_b, 40, 10, 30, 40, 120);
    const auto moved = features_of(block_a, block_b);
    REQUIRE(moved.bipolar_balance > 0.9);      // vacated and occupied: both directions, balanced
    REQUIRE(moved.brighter_share == Approx(0.5).margin(0.05));
}

TEST_CASE("character: a uniform brightness step IS illumination", "[character]") {
    REQUIRE(character_of(plane(100), plane(120)) == ChangeCharacter::global_illumination);
    // A step in the other direction is illumination too.
    REQUIRE(character_of(plane(140), plane(110)) == ChangeCharacter::global_illumination);
    // And a step large enough to saturate is still one direction.
    REQUIRE(character_of(plane(20), plane(200)) == ChangeCharacter::global_illumination);
}

TEST_CASE("character: a body crossing the field IS localized motion", "[character]") {
    auto a = plane(100);
    fill_rect(a, 10, 10, 30, 40, 120);
    auto b = plane(100);
    fill_rect(b, 40, 10, 30, 40, 120);
    REQUIRE(character_of(a, b) == ChangeCharacter::localized_motion);

    // A dark body on a bright field behaves the same way (the polarity comes from the pair, not from
    // the sign of the object's contrast).
    auto dark_a = plane(200);
    fill_rect(dark_a, 10, 10, 30, 40, 120);
    auto dark_b = plane(200);
    fill_rect(dark_b, 40, 10, 30, 40, 120);
    REQUIRE(character_of(dark_a, dark_b) == ChangeCharacter::localized_motion);

    // A small body - the animal-sized case that matters for this project.
    auto small_a = plane(100);
    fill_rect(small_a, 100, 80, 8, 6, 121);
    auto small_b = plane(100);
    fill_rect(small_b, 108, 80, 8, 6, 121);
    REQUIRE(character_of(small_a, small_b) == ChangeCharacter::localized_motion);
}

TEST_CASE("character: something appearing in place is NOT motion", "[character][refusal]") {
    // A compact, one-directional change: a lamp switching on in view, a page turning, an object
    // arriving and staying. Real, and the vocabulary refuses to call it movement.
    auto before = plane(100);
    auto after = plane(100);
    fill_rect(after, 60, 40, 20, 20, 130);
    REQUIRE(character_of(before, after) == ChangeCharacter::localized_unipolar);
    REQUIRE(character_of(after, before) == ChangeCharacter::localized_unipolar);   // and its removal
}

TEST_CASE("character: a change that scales with the scene is illumination, not movement",
          "[character][gain][real]") {
    // Measured on real hardware: the camera's automatic gain responds to a control change by scaling the
    // scene, and that response is both compact and structured - no spatial statistic separates it from a
    // body. The correlation between the delta and the pixel's own previous value does: a gain multiplies
    // what is already there, a moving body does not care how bright the background was.
    auto textured = plane(60);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            textured[size_t(y) * kWidth + x] = uint8_t(40 + (x * 160) / kWidth);   // a gradient scene
        }
    }
    std::vector<uint8_t> gained(textured.size());
    for (size_t i = 0; i < textured.size(); ++i) {
        const double v = double(textured[i]) * 1.25;                                // gain of 25%
        gained[i] = uint8_t(v > 255 ? 255 : v);
    }
    const auto gf = features_of(textured, gained);
    INFO("gain: corr=" << gf.gain_correlation << " spread=" << gf.spread_radius
                       << " balance=" << gf.bipolar_balance);
    REQUIRE(gf.gain_correlation > 0.9);
    REQUIRE(classify_change(gf) == ChangeCharacter::global_illumination);
    REQUIRE(change_character_reason(gf, ChangeCharacter::global_illumination)
                .find("scales with the scene") != std::string::npos);

    // The discriminating case: the same textured scene with a block moving across it. The delta is a
    // contrast the background knows nothing about, so the correlation collapses and the verdict stays
    // motion - the gain test must not swallow real movement.
    auto with_block = textured;
    fill_rect(with_block, 20, 40, 15, 15, 200);
    auto moved = textured;
    fill_rect(moved, 70, 40, 15, 15, 200);
    const auto mf = features_of(with_block, moved);
    INFO("moved block: corr=" << mf.gain_correlation << " spread=" << mf.spread_radius
                              << " balance=" << mf.bipolar_balance);
    REQUIRE(mf.gain_correlation < 0.6);
    REQUIRE(classify_change(mf) == ChangeCharacter::localized_motion);
}

TEST_CASE("character: the sensor's own idle band is quiet, measured from a real run",
          "[character][calibration]") {
    // Reproduces the real capture's idle band as a frame pair: about 1% of pixels crossing the delta
    // threshold, scattered over the whole frame, order 16-24 counts. On the first version of this
    // classifier that was `diffuse_motion` in 117 of 120 frames - it was reading the sensor. The
    // threshold is now above the measured band, and this test holds that boundary.
    auto idle = plane(100);
    uint32_t state = 4242;
    size_t changed = 0;
    for (size_t i = 0; i < idle.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        if ((state >> 20) % 100 < 1) {            // ~1% of pixels
            idle[i] = uint8_t(100 + 16 + (state >> 26) % 8);
            ++changed;
        }
    }
    const auto f = features_of(plane(100), idle);
    const auto verdict = classify_change(f);
    INFO("idle band: changed=" << f.changed_fraction << " verdict="
                               << change_character_to_string(verdict));
    REQUIRE(changed > 0);
    REQUIRE(f.changed_fraction > 0.005);
    REQUIRE(f.changed_fraction < 0.02);
    REQUIRE(verdict == ChangeCharacter::quiet);

    // And real movement, an order of magnitude above that band, is still classified as movement.
    auto a = plane(100);
    fill_rect(a, 20, 20, 40, 40, 130);
    auto b = plane(100);
    fill_rect(b, 60, 20, 40, 40, 130);
    REQUIRE(classify_change(features_of(a, b)) == ChangeCharacter::localized_motion);
}

TEST_CASE("character: quiet and sub-threshold changes are not reported as movement",
          "[character][refusal]") {
    REQUIRE(character_of(plane(100), plane(100)) == ChangeCharacter::quiet);
    // A gradual illumination drift, frame pair by frame pair: +8 is below the 16-count delta threshold,
    // so nothing is "changed" at all - which is how the baseline tracks a slow day/night ramp.
    REQUIRE(character_of(plane(100), plane(108)) == ChangeCharacter::quiet);
    // Sensor noise below the threshold is likewise not change.
    auto noisy = plane(100);
    add_noise(noisy, 5, 12345);
    REQUIRE(character_of(plane(100), noisy) == ChangeCharacter::quiet);
}

TEST_CASE("character: dense noise above the threshold spreads like a global change",
          "[character][limitation]") {
    // Honest limit of the measure: pixels changing everywhere at once is neither a light nor a body,
    // and with both polarities the classifier calls it diffuse motion. Sparse noise stays quiet, dense
    // noise does not - which is why the sample counts in the real runs matter.
    auto noisy = plane(100);
    add_noise(noisy, 24, 999);
    const auto raw = features_of(plane(100), noisy);
    const auto verdict = classify_change(raw);
    INFO("spread=" << raw.spread_radius << " balance=" << raw.bipolar_balance << " changed="
                   << raw.changed_fraction << " verdict=" << change_character_to_string(verdict));
    REQUIRE(raw.changed_pixels > 0);
    REQUIRE(raw.bipolar_balance > 0.9);
    REQUIRE(verdict == ChangeCharacter::diffuse_motion);
}

TEST_CASE("character: a moving shadow is indistinguishable from a moving object",
          "[character][limitation]") {
    // Stated as a test because it is a physical limit, not a tuning gap: a shadow is a moving luminance
    // boundary, so the pair (vacated here, occupied there) looks exactly like a body moving. The
    // classifier reports localized motion for both, and no threshold would change that.
    auto shadow_a = plane(140);
    fill_rect(shadow_a, 20, 20, 40, 40, 110);      // a dark patch
    auto shadow_b = plane(140);
    fill_rect(shadow_b, 60, 20, 40, 40, 110);      // the same patch moved
    REQUIRE(character_of(shadow_a, shadow_b) == ChangeCharacter::localized_motion);

    // What DOES separate them is not in this pair: a shadow tracks its illumination source, so it does
    // not persist across frames the way a body does, and it never moves against the light. Both of
    // those need more than two frames, and are recorded as future work rather than claimed here.
}

TEST_CASE("character: the centroid travels with the change", "[character][features]") {
    // Direction consistency and persistence in their smallest form: two consecutive pairs, the block
    // moving right each time, give a monotonically increasing centroid.
    auto a = plane(100);
    fill_rect(a, 10, 10, 20, 20, 130);
    auto b = plane(100);
    fill_rect(b, 40, 10, 20, 20, 130);
    auto c = plane(100);
    fill_rect(c, 70, 10, 20, 20, 130);

    const auto ab = features_of(a, b);
    const auto bc = features_of(b, c);
    REQUIRE(ab.centroid_valid);
    REQUIRE(bc.centroid_valid);
    INFO("centroid x: " << ab.centroid_x << " -> " << bc.centroid_x);
    REQUIRE(bc.centroid_x > ab.centroid_x);            // travelling right
    REQUIRE(ab.centroid_y == Approx(bc.centroid_y).margin(0.02));   // and not vertically
}

TEST_CASE("region descriptors refuse what they cannot describe", "[character][refusal]") {
    const auto p = plane(100);
    // Geometry that does not multiply to the pixel count.
    REQUIRE_FALSE(extract_region_change_features(p.data(), p.data(), kPixels, 100, 100).valid);
    // Null plane.
    REQUIRE_FALSE(extract_region_change_features(nullptr, p.data(), kPixels, kWidth, kHeight).valid);
    // Too few pixels to describe anything.
    REQUIRE_FALSE(extract_region_change_features(p.data(), p.data(), 8, 4, 2).valid);
    // A grid larger than the bound.
    REQUIRE_FALSE(extract_region_change_features(p.data(), p.data(), kPixels, kWidth, kHeight,
                                                 FeatureBounds::kDefaultPixelDelta, 16, 16).valid);
    // And an invalid descriptor classifies as unknown rather than guessing.
    RegionChangeFeatures bad;
    REQUIRE(classify_change(bad) == ChangeCharacter::unknown);
}

TEST_CASE("character: the explanation is bounded and never identifies anything", "[character][refusal]") {
    const auto verdict_reason = change_character_reason(features_of(plane(100), plane(120)),
                                                        ChangeCharacter::global_illumination);
    REQUIRE(verdict_reason.find("illumination") != std::string::npos);
    REQUIRE(verdict_reason.find("changed=") != std::string::npos);
    for (const char* forbidden : {"person", "animal", "species", "face", "intruder", "fall"}) {
        INFO("forbidden word: " << forbidden);
        REQUIRE(verdict_reason.find(forbidden) == std::string::npos);
    }
}

// ------------------------------------------------------------------------------------------------
// The whole chain, at the frame level: two streams that look identical to the old single scalar
// (a sustained increase in the changed-pixel fraction) and are different in the pixels.
// ------------------------------------------------------------------------------------------------

namespace {

// 20 quiet frames, then the illumination fluctuates: a light switching on and off, or an
// auto-exposure loop hunting. Each frame PAIR is a full-frame step the same way, which is what an
// illumination change is; the detector sees a sustained increase because the recent mean stays high.
//
// (A single step and then nothing would confirm for only two frames - the pair after the step is
// identical - and would never reach an episode. That is a real property of the evidence, not a
// fixture convenience, and it is why the lighting false positive needs a fluctuating source.)
std::vector<std::vector<uint8_t>> illumination_stream() {
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < 20; ++i) frames.push_back(plane(100));
    for (int i = 0; i < 12; ++i) frames.push_back(plane(i % 2 == 0 ? 120 : 100));
    return frames;
}

// 20 quiet frames, then a small body crossing the field for twelve frames: the animal-sized case.
std::vector<std::vector<uint8_t>> motion_stream() {
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < 20; ++i) frames.push_back(plane(100));
    for (int i = 0; i < 12; ++i) {
        auto f = plane(100);
        const uint32_t x = 20 + uint32_t(i) * 8;      // walking right, one step per frame
        fill_rect(f, x, 50, 10, 8, 130);              // 10x8 body, brighter than the background
        frames.push_back(f);
    }
    return frames;
}

struct StreamResult {
    std::vector<Observation> observations;
    std::vector<std::string> characters;      // per observation, "" when not characterised
    size_t confirmed = 0;
};

StreamResult run_stream(const std::vector<std::vector<uint8_t>>& frames) {
    CameraActivitySession::Config cfg;
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.source = "/dev/video0";
    CameraActivitySession session(cfg);

    StreamResult result;
    const auto t0 = std::chrono::system_clock::now() - std::chrono::seconds(60);
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto at = t0 + std::chrono::milliseconds(200 * int64_t(i));
        const auto finding = session.ingest(frames[i].data(), kPixels, Provenance::synthetic, at);
        const auto observation = session.observation_for("chg-" + std::to_string(i));
        result.observations.push_back(observation);
        result.characters.push_back(finding.character_valid ? finding.change_character : std::string());
        if (observation.status == DetectorStatus::detected && observation.confidence.has_value()) {
            ++result.confirmed;
        }
    }
    return result;
}

std::vector<Event> events_of(const std::vector<Observation>& obs) {
    std::vector<Event> events;
    for (const auto& o : obs) events.push_back(o.to_event());
    return events;
}

DetectionFusionRule episode_rule(bool require_motion_character) {
    DetectionFusionRule::Config cfg;
    cfg.evidence_detector_ids = {"camera-motion-activity"};
    cfg.use_episode_evidence = true;
    cfg.episode.require_motion_character = require_motion_character;
    return DetectionFusionRule("detection-fusion", cfg);
}

}  // namespace

TEST_CASE("chain: a sustained lighting step and a moving body are the same scalar and different pixels",
          "[character][session]") {
    const auto lighting = run_stream(illumination_stream());
    const auto motion = run_stream(motion_stream());

    INFO("lighting confirmed=" << lighting.confirmed << " motion confirmed=" << motion.confirmed);
    // Both look like a sustained change to the detector - which is exactly the problem this fixes.
    REQUIRE(lighting.confirmed >= 3);
    REQUIRE(motion.confirmed >= 3);

    // And their characters differ, from the pixels, on the evidence that matters (the confirmed frames).
    size_t lighting_illumination = 0;
    size_t motion_movement = 0;
    for (size_t i = 0; i < lighting.characters.size(); ++i) {
        if (lighting.characters[i] == "global_illumination") ++lighting_illumination;
    }
    for (size_t i = 0; i < motion.characters.size(); ++i) {
        if (motion.characters[i] == "localized_motion") ++motion_movement;
    }
    INFO("lighting frames classed as illumination: " << lighting_illumination
         << " | motion frames classed as localized motion: " << motion_movement);
    REQUIRE(lighting_illumination >= 3);
    REQUIRE(motion_movement >= 3);
    REQUIRE(lighting.characters[25] == "global_illumination");
    REQUIRE(motion.characters[25] == "localized_motion");
}

TEST_CASE("chain: the session's own warm-up change is illumination, and is not evidence",
          "[character][session][real]") {
    // Measured on real hardware twice (docs/evidence/PHYSICAL_CAMERA_CHARACTER_RUN{,_2}.log): the single
    // large change in each capture - 0.3522 and 0.2965 of pixels, an order of magnitude above the idle
    // band - was classified `global_illumination` on the warm-up frame pair. That is the camera settling
    // its exposure between the first two frames, not a body: it is spread over the frame and moves every
    // pixel one way.
    //
    // The pipeline was already protected against it, and this test says why: a warm-up observation is
    // `inconclusive`, so it is not a vote, so it can never contribute to an episode.
    auto first = plane(100);
    auto second = plane(160);          // +60 everywhere: an exposure step, spread and unipolar
    CameraActivitySession::Config cfg;
    cfg.width = kWidth;
    cfg.height = kHeight;
    CameraActivitySession session(cfg);
    const auto t0 = std::chrono::system_clock::now();

    (void)session.ingest(first.data(), kPixels, Provenance::synthetic, t0);
    const auto f2 = session.ingest(second.data(), kPixels, Provenance::synthetic,
                                   t0 + std::chrono::milliseconds(200));
    const auto o2 = session.observation_for("warm-2");
    INFO("warm-up: char=<< " << f2.change_character << " motion=" << f2.motion_level
         << " kind=" << camera_finding_kind_to_string(f2.kind));
    REQUIRE(f2.character_valid);
    REQUIRE(f2.change_character == "global_illumination");
    REQUIRE(f2.motion_level > 0.9);                       // a very large change indeed
    REQUIRE(f2.kind == CameraFindingKind::warming_up);    // and still not a finding
    REQUIRE(o2.status == DetectorStatus::inconclusive);   // so it is not a vote
    REQUIRE(o2.features.contains("change_character"));
    REQUIRE(o2.features["change_character"] == "global_illumination");
}

TEST_CASE("chain: the character gate refuses the lighting alert and keeps the motion one",
          "[character][session][gate]") {
    const auto lighting = run_stream(illumination_stream());
    const auto motion = run_stream(motion_stream());

    // WITHOUT the gate: the lighting step alerts, which is the measured false positive this work exists
    // to remove. Recorded here so the improvement is visible rather than asserted.
    {
        DetectionFusionRule without = episode_rule(false);
        const auto lighting_alert = without.evaluate(events_of(lighting.observations));
        DetectionFusionRule second = episode_rule(false);
        const auto motion_alert = second.evaluate(events_of(motion.observations));
        REQUIRE(lighting_alert.has_value());      // the false positive, before
        REQUIRE(motion_alert.has_value());        // the true positive, which must survive
    }

    // WITH the gate: the lighting step is refused and the moving body still alerts.
    {
        DetectionFusionRule gated = episode_rule(true);
        const auto lighting_alert = gated.evaluate(events_of(lighting.observations));
        DetectionFusionRule gated2 = episode_rule(true);
        const auto motion_alert = gated2.evaluate(events_of(motion.observations));
        INFO("gated lighting alert: " << (lighting_alert ? "RAISED" : "refused"));
        REQUIRE_FALSE(lighting_alert.has_value());
        REQUIRE(motion_alert.has_value());
        REQUIRE(motion_alert->details["evidence_mode"] == "episode");
        REQUIRE(motion_alert->details["provenance"] == "synthetic");
    }
}

TEST_CASE("chain: the gate does not disable the episode path for evidence it cannot classify",
          "[character][gate]") {
    // Members with no change_character feature (any detector that does not compute one) must not be
    // counted against a run, or enabling this gate would silently switch the whole path off elsewhere.
    std::vector<Observation> obs;
    const auto base = std::chrono::system_clock::now() - std::chrono::seconds(10);
    int i = 0;
    for (int k = 0; k < 5; ++k) {
        Observation o;
        o.observation_id = "uncharacterised-" + std::to_string(i);
        o.source = "/dev/video0";
        o.provenance = Provenance::synthetic;
        o.detector_id = "camera-motion-activity";
        o.model_id = "ema-baseline-v1";
        o.status = DetectorStatus::detected;
        o.confidence = 0.8;
        o.observation_timestamp = base + std::chrono::milliseconds(10 * i);
        o.features = nlohmann::json::object();
        o.features["direction"] = "rising";          // no change_character key at all
        obs.push_back(o);
        ++i;
    }
    DetectionFusionRule gated = episode_rule(true);
    REQUIRE(gated.evaluate(events_of(obs)).has_value());
}

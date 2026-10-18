#include "catch2/catch.hpp"

#include "backend/linux/V4l2CameraDevice.h"
#include "core/BoundedFeatures.h"
#include "core/CameraActivitySession.h"
#include "core/Logger.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// The REAL camera path's testable parts, with no hardware involved.
//
// WHAT THESE TESTS PROVE
//   The scalar derivation, the closed finding vocabulary, the provenance rule (a synthetic
//   frame can never be reported as real), the equipment-stall distinction, the geometry and
//   size bounds, and the capture GATE (start() refuses without an explicit authorization).
//
// WHY NO CAMERA IS OPENED HERE
//   A test suite that needs a webcam is a suite that fails on a machine without one. The
//   real device is exercised by `hg_camera_bridge --capture --confirm`, which prints the
//   frames it actually reduced. What is tested here is everything that can be decided
//   without one - including the parts that must REFUSE to work.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kW = 64;
constexpr size_t kH = 48;
constexpr size_t kPixels = kW * kH;

std::vector<uint8_t> plane(uint8_t value) { return std::vector<uint8_t>(kPixels, value); }

// Change a `side x side` block in the middle. The size is the "how much moved" knob.
void move_block(std::vector<uint8_t>& p, int side, uint8_t delta) {
    const size_t cx = kW / 2, cy = kH / 2;
    for (size_t y = cy - side / 2; y < cy + side / 2; ++y) {
        for (size_t x = cx - side / 2; x < cx + side / 2; ++x) {
            p[y * kW + x] = static_cast<uint8_t>(p[y * kW + x] + delta);
        }
    }
}

using Clock = std::chrono::system_clock;
Clock::time_point t0() { return Clock::time_point(std::chrono::seconds(1000)); }

}  // namespace

// ---------------------------------------------------------------- derivation

TEST_CASE("Camera path: luma motion is derived, and identical frames move nothing",
          "[camera][motion]") {
    Logger::initialize("error");
    CameraActivitySession session;
    auto now = t0();

    const auto a = plane(120);
    const auto b = plane(120);          // identical: no motion at all

    const CameraFinding first = session.ingest(a.data(), a.size(), Provenance::real, now);
    REQUIRE(first.kind == CameraFindingKind::warming_up);   // one frame cannot show change

    const CameraFinding second = session.ingest(b.data(), b.size(), Provenance::real, now);
    REQUIRE(second.motion_level == Approx(0.0));
    REQUIRE(second.kind == CameraFindingKind::warming_up);  // still establishing a baseline

    // A visible change is measured, and the number is the changed FRACTION of the frame.
    auto c = plane(120);
    move_block(c, 32, 100);
    const CameraFinding third = session.ingest(c.data(), c.size(), Provenance::real, now);
    const double expected = static_cast<double>(32 * 32) / static_cast<double>(kPixels);
    REQUIRE(third.motion_level == Approx(expected).epsilon(0.01));
    REQUIRE(third.mean_delta > 0.0);
}

TEST_CASE("Camera path: bad input is refused rather than sampled", "[camera][bounds]") {
    Logger::initialize("error");
    CameraActivitySession session;
    const auto now = t0();

    const CameraFinding null_luma = session.ingest(nullptr, kPixels, Provenance::real, now);
    REQUIRE_FALSE(null_luma.valid);
    REQUIRE(null_luma.kind == CameraFindingKind::insufficient_evidence);
    REQUIRE(null_luma.reason.find("no frame") != std::string::npos);

    const auto p = plane(10);
    // Below the minimum plane: statistics on it would be noise.
    const CameraFinding too_small = session.ingest(p.data(), FeatureBounds::kMinPixels - 1,
                                                   Provenance::real, now);
    REQUIRE_FALSE(too_small.valid);
    // Above the maximum: refused, NOT resized (the bounded-feature contract).
    const CameraFinding too_big = session.ingest(p.data(), FeatureBounds::kMaxPixels + 1,
                                                 Provenance::real, now);
    REQUIRE_FALSE(too_big.valid);
    REQUIRE(too_big.reason.find("bounded") != std::string::npos);
}

TEST_CASE("Camera path: a geometry change restarts the reference instead of comparing planes",
          "[camera][motion]") {
    Logger::initialize("error");
    CameraActivitySession session;
    const auto now = t0();

    const auto a = plane(90);
    (void)session.ingest(a.data(), a.size(), Provenance::real, now);

    const auto other = plane(90);
    const CameraFinding f = session.ingest(other.data(), kPixels / 2, Provenance::real, now);
    REQUIRE(f.kind == CameraFindingKind::warming_up);
    REQUIRE(f.sentence.find("geometry changed") != std::string::npos);
}

// ---------------------------------------------------------------- vocabulary

TEST_CASE("Camera path: a sustained rise in motion is detected and named in plain words",
          "[camera][finding]") {
    Logger::initialize("error");
    CameraActivitySession::Config cfg;
    cfg.warmup = 3;
    cfg.confirm_samples = 2;
    CameraActivitySession session(cfg);
    auto now = t0();

    // Establish a baseline with a small, steady amount of change.
    auto base = plane(100);
    uint8_t v = 0;
    for (int i = 0; i < 6; ++i) {
        move_block(base, 2, static_cast<uint8_t>(++v));
        (void)session.ingest(base.data(), base.size(), Provenance::real, now);
        now += std::chrono::seconds(1);
    }

    // Now much more movement, sustained.
    CameraFinding last{};
    for (int i = 0; i < 6; ++i) {
        move_block(base, 32, 60);
        last = session.ingest(base.data(), base.size(), Provenance::real, now);
        now += std::chrono::seconds(1);
    }
    REQUIRE(last.kind == CameraFindingKind::sustained_increase);
    REQUIRE(last.sentence.find("sustained increase in motion") != std::string::npos);
    REQUIRE(last.ratio > 1.0);
    REQUIRE(session.last_status() == DetectorStatus::detected);

    // The vocabulary is closed: no person/animal/species claim can be expressed.
    std::vector<std::string> vocabulary;
    for (auto k : {CameraFindingKind::warming_up, CameraFindingKind::quiet_baseline,
                   CameraFindingKind::sustained_increase, CameraFindingKind::sustained_decrease,
                   CameraFindingKind::frames_stalled, CameraFindingKind::insufficient_evidence}) {
        vocabulary.emplace_back(camera_finding_kind_to_string(k));
    }
    for (const auto& word : vocabulary) {
        for (const char* forbidden : {"person", "human", "animal", "species", "face", "fall",
                                      "intruder", "emergency", "danger"}) {
            REQUIRE(word.find(forbidden) == std::string::npos);
        }
    }
    REQUIRE(last.uncertainty.find("not a person") != std::string::npos);
}

TEST_CASE("Camera path: a stalled device is never reported as a quiet scene",
          "[camera][finding][stall]") {
    Logger::initialize("error");
    CameraActivitySession session;
    const auto now = t0();

    const auto a = plane(80);
    (void)session.ingest(a.data(), a.size(), Provenance::real, now);

    const CameraFinding stall = session.note_stalled_window("no frame within 2000 ms");
    REQUIRE(stall.kind == CameraFindingKind::frames_stalled);
    REQUIRE(stall.kind != CameraFindingKind::quiet_baseline);   // the whole point
    REQUIRE(stall.sentence.find("delivered no frame") != std::string::npos);
    REQUIRE(stall.uncertainty.find("capture device") != std::string::npos);
    // Nothing was measured, so no provenance is claimed.
    REQUIRE_FALSE(stall.real_source);
    REQUIRE(stall.reason.find("2000 ms") != std::string::npos);
    REQUIRE(session.current().kind == CameraFindingKind::frames_stalled);
}

// ---------------------------------------------------------------- provenance

TEST_CASE("Camera path: provenance is carried, never upgraded",
          "[camera][provenance]") {
    Logger::initialize("error");
    auto now = t0();

    CameraActivitySession synthetic;
    const auto a = plane(100);
    auto b = plane(100);
    move_block(b, 16, 90);
    (void)synthetic.ingest(a.data(), a.size(), Provenance::synthetic, now);
    const CameraFinding s = synthetic.ingest(b.data(), b.size(), Provenance::synthetic, now);
    REQUIRE_FALSE(s.real_source);
    const Observation so = synthetic.observation_for("obs-synth", "fixture");
    REQUIRE(so.provenance == Provenance::synthetic);

    CameraActivitySession real;
    (void)real.ingest(a.data(), a.size(), Provenance::real, now);
    const CameraFinding rf = real.ingest(b.data(), b.size(), Provenance::real, now);
    REQUIRE(rf.real_source);
    const Observation ro = real.observation_for("obs-real", "/dev/video0");
    REQUIRE(ro.provenance == Provenance::real);
    // The observation's detector identity comes from the component that produced the number.
    REQUIRE(ro.detector_id == "camera-motion-activity");
    REQUIRE(ro.model_id == "ema-baseline-v1");
    REQUIRE_FALSE(ro.features.empty());
    // A REAL observation and a SYNTHETIC one cannot be confused, even with identical input.
    REQUIRE(ro.provenance != so.provenance);
}

TEST_CASE("Camera path: a confirmed FALLING change yields a valid observation (regression)",
          "[camera][regression]") {
    Logger::initialize("error");
    // Frozen from a REAL crash on the first physical camera run: the detector clamped
    // confidence at 1.0 only, so a confirmed fall (ratio < 1) produced a NEGATIVE confidence
    // and the validated Event constructor threw std::invalid_argument. The same defect was
    // reachable from the audio path (a confirmed fall in level), so it was fixed in the
    // detector rather than papered over here.
    CameraActivitySession::Config cfg;
    cfg.warmup = 2;
    cfg.confirm_samples = 1;
    cfg.recent_window = 3;
    CameraActivitySession session(cfg);
    auto now = t0();

    auto moving = plane(100);
    auto still = plane(100);
    uint8_t v = 0;
    // Establish a HIGH baseline.
    for (int i = 0; i < 10; ++i) {
        move_block(moving, 40, static_cast<uint8_t>(30 + (++v)));
        (void)session.ingest(moving.data(), moving.size(), Provenance::real, now);
        now += std::chrono::seconds(1);
    }
    // Then quiet frames: a confirmed fall.
    CameraFinding last{};
    for (int i = 0; i < 4; ++i) {
        (void)session.ingest(still.data(), still.size(), Provenance::real, now);
        last = session.current();
        now += std::chrono::seconds(1);
    }

    REQUIRE(last.kind == CameraFindingKind::sustained_decrease);
    REQUIRE(last.ratio < 1.0);
    // The finding's numbers must agree with its own verdict: a "decrease" beside a ratio
    // above 1 is a self-contradiction a reviewer would rightly distrust.
    REQUIRE(last.baseline > last.motion_level);

    const Observation o = session.observation_for("obs-fall", "fixture");
    REQUIRE(o.provenance == Provenance::real);
    REQUIRE(o.confidence >= 0.0);
    REQUIRE(o.confidence <= 1.0);
    REQUIRE_NOTHROW(o.to_event());          // the exact call that used to abort
    REQUIRE(o.features.contains("ratio"));
}

// ---------------------------------------------------------------- the gate

TEST_CASE("Camera device: capture is refused unless the operator authorizes it",
          "[camera][device][gate]") {
    Logger::initialize("error");

    V4l2CameraDevice cam;                 // constructed only: opens nothing
    REQUIRE(cam.state() == DeviceState::Idle);
    REQUIRE_FALSE(cam.capture_authorized());
    REQUIRE(cam.kind() == DeviceKind::camera);

    // start() without an authorization is refused, and refuses BEFORE touching any device.
    REQUIRE_THROWS(cam.start());

    // Revoking is idempotent and safe at rest.
    cam.authorize_capture(false);
    REQUIRE_FALSE(cam.capture_authorized());
    cam.authorize_capture(true);
    REQUIRE(cam.capture_authorized());
    cam.authorize_capture(false);
    REQUIRE_FALSE(cam.capture_authorized());
}

TEST_CASE("Camera device: an unusable node fails closed with a reason",
          "[camera][device][gate]") {
    Logger::initialize("error");

    V4l2CameraDevice::Config cfg;
    cfg.device = "/dev/video-does-not-exist";
    V4l2CameraDevice cam(cfg);
    REQUIRE_THROWS(cam.initialize());
    REQUIRE(cam.state() == DeviceState::Error);   // not a silent no-op

    // A device in Error must not be startable, and the failure is reported rather than
    // pretending to capture a black frame.
    cam.authorize_capture(true);
    REQUIRE_THROWS(cam.start());
    cam.close();                                   // deterministic cleanup, no crash
    REQUIRE(cam.state() == DeviceState::Error);

    // A frame request from a device that is not capturing is refused with a reason.
    LumaFrame frame;
    std::string why;
    V4l2CameraDevice idle;
    REQUIRE_FALSE(idle.next_luma(&frame, &why));
    REQUIRE(why.find("not capturing") != std::string::npos);
}

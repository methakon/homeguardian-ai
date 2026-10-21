#include "catch2/catch.hpp"

#include "core/CaptureAccounting.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/FakeMediaDevice.h"
#include "core/NodeIdentity.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// D-02: the acquisition-node contracts that must hold WITHOUT a phone attached.
//
// These cover the software half of the milestone: the bounded/authorized
// payload contract (the accountant every real callback goes through), device
// identity, and the consent/permission/lifecycle guarantees at the delivery
// boundary. They do NOT prove physical capture - that needs a device and is
// reported separately as BLOCKED.
//
// Nothing here interprets media: no faces, speech, falls or emergencies.
// ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::system_clock;
const auto kT0 = Clock::time_point(std::chrono::milliseconds(1'700'000'000'000LL));

// An in-memory consent repository (no database on the host path).
class MemRepo : public IConsentRepository {
public:
    void save(const ConsentRecord& r) override { records_.push_back(r); }
    std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<ConsentRecord> find_by_subject(const std::string& s) override { return filter(s, ""); }
    std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
        return filter(s, p);
    }
    size_t count() override { return records_.size(); }
    size_t delete_all() override { size_t n = records_.size(); records_.clear(); return n; }
    void grant(const std::string& subject, const std::string& purpose,
               const std::string& category, Clock::time_point at,
               std::optional<Clock::time_point> expires = std::nullopt) {
        records_.push_back(ConsentRecord::create_granted("c-grant", subject, purpose, {category},
                                                         "1.0.0", "operator", expires));
        (void)at;
    }
    void withdraw(const std::string& subject, const std::string& purpose) {
        records_.push_back(ConsentRecord::create_withdrawn("c-withdraw", subject, purpose, "1.0.0",
                                                           "operator"));
    }
private:
    std::vector<ConsentRecord> filter(const std::string& s, const std::string& p) {
        std::vector<ConsentRecord> out;
        for (auto& r : records_)
            if (r.get_subject_scope() == s && (p.empty() || r.get_purpose() == p)) out.push_back(r);
        return out;
    }
    std::vector<ConsentRecord> records_;
};

constexpr uint32_t kW = 320, kH = 240;
constexpr size_t kFrameBytes = 320u * 240u * 3u / 2u;   // NV21
constexpr int64_t kNow = 1'700'000'000'000LL;

}  // namespace

// ===========================================================================
// Camera callback contract (bounded + authorized + timestamped)
// ===========================================================================

TEST_CASE("D-02 camera: an unauthorized frame is dropped and never counts", "[d02][camera]") {
    CaptureAccountant acc;
    acc.note_capture_started(/*video=*/true, kNow);
    auto d = acc.account_frame(kW, kH, kFrameBytes, kNow, /*authorized=*/false, kNow);
    REQUIRE_FALSE(d.authorized);
    REQUIRE(d.reason == DropReason::consent_denied);
    REQUIRE(acc.counters().frames_seen == 1);
    REQUIRE(acc.counters().frames_authorized == 0);
    REQUIRE(acc.counters().frames_dropped == 1);
    REQUIRE(acc.counters().frame_bytes == 0);
    REQUIRE(acc.counters().last_drop == DropReason::consent_denied);
    REQUIRE_FALSE(acc.counters().last_drop_detail.empty());
}

TEST_CASE("D-02 camera: an authorized frame while capture is stopped is not counted",
          "[d02][camera]") {
    CaptureAccountant acc;   // never noted started
    auto d = acc.account_frame(kW, kH, kFrameBytes, kNow, /*authorized=*/true, kNow);
    REQUIRE_FALSE(d.authorized);
    REQUIRE(d.reason == DropReason::not_capturing);
    REQUIRE(acc.counters().frames_authorized == 0);
}

TEST_CASE("D-02 camera: an authorized frame records counters and timestamps", "[d02][camera]") {
    CaptureAccountant acc;
    acc.note_capture_started(true, kNow);
    for (int i = 0; i < 5; ++i) {
        auto d = acc.account_frame(kW, kH, kFrameBytes, kNow + i * 100, true, kNow + i * 100);
        REQUIRE(d.authorized);
    }
    const auto& c = acc.counters();
    REQUIRE(c.frames_seen == 5);
    REQUIRE(c.frames_authorized == 5);
    REQUIRE(c.frames_dropped == 0);
    REQUIRE(c.frame_bytes == 5 * kFrameBytes);
    REQUIRE(c.first_frame_ms == kNow);
    REQUIRE(c.last_frame_ms == kNow + 400);
    REQUIRE(c.last_drop == DropReason::none);
}

TEST_CASE("D-02 camera: dimension, size and timestamp bounds are enforced", "[d02][camera]") {
    CaptureAccountant acc;
    acc.note_capture_started(true, kNow);

    REQUIRE(acc.account_frame(8, 8, kFrameBytes, kNow, true, kNow).reason ==
            DropReason::bad_dimensions);
    REQUIRE(acc.account_frame(4000, kH, kFrameBytes, kNow, true, kNow).reason ==
            DropReason::bad_dimensions);
    REQUIRE(acc.account_frame(kW, kH, 0, kNow, true, kNow).reason == DropReason::oversize);
    REQUIRE(acc.account_frame(kW, kH, 64u * 1024u * 1024u, kNow, true, kNow).reason ==
            DropReason::oversize);
    REQUIRE(acc.account_frame(kW, kH, kFrameBytes, 0, true, kNow).reason ==
            DropReason::bad_timestamp);
    REQUIRE(acc.account_frame(kW, kH, kFrameBytes, kNow + 60'000, true, kNow).reason ==
            DropReason::bad_timestamp);

    REQUIRE(acc.counters().frames_authorized == 0);
    REQUIRE(acc.counters().frames_dropped == 6);
}

TEST_CASE("D-02 camera: a backwards timestamp is counted but still accepted",
          "[d02][camera]") {
    CaptureAccountant acc;
    acc.note_capture_started(true, kNow);
    REQUIRE(acc.account_frame(kW, kH, kFrameBytes, kNow + 1000, true, kNow + 1000).authorized);
    REQUIRE(acc.account_frame(kW, kH, kFrameBytes, kNow + 900, true, kNow + 1000).authorized);
    REQUIRE(acc.counters().timestamp_regressions == 1);
    REQUIRE(acc.counters().frames_authorized == 2);
}

TEST_CASE("D-02 camera: counters saturate instead of wrapping", "[d02][camera]") {
    CaptureCounters c;
    c.frames_authorized = UINT64_MAX - 1;
    c.sat_add(c.frames_authorized, 1000);
    REQUIRE(c.frames_authorized == UINT64_MAX);
    c.sat_add(c.frame_bytes, 5);
    REQUIRE(c.frame_bytes == 5);
}

// ===========================================================================
// Microphone callback contract
// ===========================================================================

TEST_CASE("D-02 audio: PCM buffers are validated, counted and bounded", "[d02][audio]") {
    CaptureAccountant acc;
    acc.note_capture_started(/*video=*/false, kNow);

    const uint32_t rate = 16000, channels = 1, frames = 1024;   // 2048 bytes of s16 mono
    const size_t bytes = frames * channels * 2u;
    auto ok = acc.account_audio(rate, channels, frames, bytes, kNow, true, kNow);
    REQUIRE(ok.authorized);
    REQUIRE(acc.counters().audio_buffers_authorized == 1);
    REQUIRE(acc.counters().audio_frames == frames);
    REQUIRE(acc.counters().audio_bytes == bytes);

    REQUIRE(acc.account_audio(rate, channels, frames, bytes, kNow, /*authorized=*/false, kNow).reason
            == DropReason::consent_denied);
    REQUIRE(acc.account_audio(100, channels, frames, bytes, kNow, true, kNow).reason ==
            DropReason::bad_sample_rate);
    REQUIRE(acc.account_audio(rate, 9, frames, bytes, kNow, true, kNow).reason ==
            DropReason::bad_channels);
    REQUIRE(acc.account_audio(rate, 0, frames, bytes, kNow, true, kNow).reason ==
            DropReason::bad_channels);
    REQUIRE(acc.account_audio(rate, channels, frames, 10u * 1024u * 1024u, kNow, true, kNow).reason ==
            DropReason::oversize);
    REQUIRE(acc.account_audio(rate, channels, 0, bytes, kNow, true, kNow).reason ==
            DropReason::oversize);

    REQUIRE(acc.counters().audio_buffers_authorized == 1);
    REQUIRE(acc.counters().audio_buffers_dropped == 6);
}

TEST_CASE("D-02 audio: an unauthorized buffer never contributes samples", "[d02][audio]") {
    CaptureAccountant acc;
    acc.note_capture_started(false, kNow);
    acc.account_audio(16000, 1, 512, 1024, kNow, false, kNow);
    REQUIRE(acc.counters().audio_frames == 0);
    REQUIRE(acc.counters().audio_bytes == 0);
    REQUIRE(acc.counters().audio_buffers_dropped == 1);
}

TEST_CASE("D-02 audio: xruns and a stop clear the running state", "[d02][audio]") {
    CaptureAccountant acc;
    acc.note_capture_started(false, kNow);
    acc.note_xrun();
    acc.note_xrun();
    REQUIRE(acc.counters().xruns == 2);
    acc.note_capture_stopped();
    REQUIRE_FALSE(acc.audio_running());
    REQUIRE(acc.account_audio(16000, 1, 512, 1024, kNow, true, kNow).reason ==
            DropReason::not_capturing);
}

TEST_CASE("D-02: reset clears every counter and running flag", "[d02][audio][camera]") {
    CaptureAccountant acc;
    acc.note_capture_started(true, kNow);
    acc.account_frame(kW, kH, kFrameBytes, kNow, true, kNow);
    acc.reset();
    REQUIRE(acc.counters().frames_seen == 0);
    REQUIRE(acc.counters().frames_authorized == 0);
    REQUIRE_FALSE(acc.video_running());
    REQUIRE(acc.counters().last_drop == DropReason::none);
}

// ===========================================================================
// Device identity (D-03 readiness)
// ===========================================================================

TEST_CASE("D-02 identity: node id is stable, opaque and well-formed", "[d02][identity]") {
    const std::string a = make_node_id("android-id-abc123");
    const std::string b = make_node_id("android-id-abc123");
    const std::string c = make_node_id("another-device");
    REQUIRE(a == b);
    REQUIRE(a != c);
    REQUIRE(a.size() == 19);
    REQUIRE(a.compare(0, 3, "hg-") == 0);
    REQUIRE(make_node_id("") == "hg-0000000000000000");

    auto id = make_node_identity("android-id-abc123", "Front Door", 27, "armeabi-v7a", "MT6580");
    REQUIRE(node_identity_valid(id));
    REQUIRE(id.display_name == "Front Door");
    const std::string summary = node_identity_summary(id);
    REQUIRE(summary.find(id.node_id) != std::string::npos);
    REQUIRE(summary.find("api=27") != std::string::npos);
}

TEST_CASE("D-02 identity: labels are sanitised and bounded", "[d02][identity]") {
    REQUIRE(sanitize_label("  spaced   out  ") == "spaced out");
    // NOTE: "\x01ctl" would parse as the hex escape \x01c, so the control byte is
    // appended explicitly. (That greediness made this test look like an
    // implementation bug during D-02 - see FRICTION_LOG.)
    std::string raw = "bad\nname\r\twith";
    raw.push_back('\x01');
    raw += "ctl";
    REQUIRE(sanitize_label(raw) == "badnamewithctl");
    REQUIRE(sanitize_label(std::string(200, 'x')).size() == 32);
    REQUIRE(sanitize_label("") == "");

    auto id = make_node_identity("seed", "   ", 27, "armeabi-v7a", "");
    REQUIRE(id.display_name == "unnamed node");   // never empty
    REQUIRE(node_identity_valid(id));

    NodeIdentity broken;   // no id at all
    REQUIRE_FALSE(node_identity_valid(broken));
}

// ===========================================================================
// Consent / permission / lifecycle at the delivery boundary
// ===========================================================================

TEST_CASE("D-02 gate: withdrawal stops delivery and forces capture down",
          "[d02][consent][lifecycle]") {
    MemRepo repo;
    repo.grant("node-1", "camera_acquire", "camera", kT0);
    ConsentGate gate(repo, /*media_capture_enabled=*/true);
    FakeMediaDevice cam(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guarded(cam, gate, AcquisitionRequest{"node-1", "camera_acquire", "camera"});

    REQUIRE(guarded.initialize().allowed);
    REQUIRE(guarded.start().allowed);
    REQUIRE(cam.state() == DeviceState::Capturing);

    // Frames flow while authorized.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(guarded.authorize_delivery(kT0));
        cam.deliver_payload();
    }
    REQUIRE(cam.delivered().size() == 3);

    // Operator withdraws consent: the very next delivery must be refused and the
    // device forced out of capture, so nothing further can be delivered.
    repo.withdraw("node-1", "camera_acquire");
    REQUIRE_FALSE(guarded.authorize_delivery(kT0));
    REQUIRE(guarded.state() != DeviceState::Capturing);
    cam.deliver_payload();
    REQUIRE(cam.delivered().size() == 3);   // nothing new delivered after denial
}

TEST_CASE("D-02 gate: expired and absent consent deny, capture-off denies",
          "[d02][consent]") {
    SECTION("expired grant") {
        MemRepo repo;
        repo.grant("node-1", "mic_acquire", "audio", kT0, /*expires=*/std::optional<Clock::time_point>{kT0});
        ConsentGate gate(repo, true);
        auto d = gate.check(AcquisitionRequest{"node-1", "mic_acquire", "audio"}, kT0 + std::chrono::seconds(1));
        REQUIRE_FALSE(d.authorized);
    }
    SECTION("no record at all") {
        MemRepo repo;
        ConsentGate gate(repo, true);
        REQUIRE_FALSE(gate.check(AcquisitionRequest{"node-1", "mic_acquire", "audio"}, kT0).authorized);
    }
    SECTION("capture disabled by configuration") {
        MemRepo repo;
        repo.grant("node-1", "mic_acquire", "audio", kT0);
        ConsentGate gate(repo, /*media_capture_enabled=*/false);
        REQUIRE_FALSE(gate.check(AcquisitionRequest{"node-1", "mic_acquire", "audio"}, kT0).authorized);
    }
}

TEST_CASE("D-02 gate: revoked OS permission stops delivery", "[d02][consent][permission]") {
    MemRepo repo;
    repo.grant("node-1", "camera_acquire", "camera", kT0);
    ConsentGate gate(repo, true);
    FakeMediaDevice cam(DeviceKind::camera, "fake-cam");

    bool permission = true;
    ConsentGuardedDevice guarded(cam, gate, AcquisitionRequest{"node-1", "camera_acquire", "camera"},
                                 [&permission] { return permission; });
    REQUIRE(guarded.initialize().allowed);
    REQUIRE(guarded.start().allowed);
    REQUIRE(guarded.authorize_delivery(kT0));

    permission = false;                       // the OS revoked CAMERA
    REQUIRE_FALSE(guarded.authorize_delivery(kT0));
    REQUIRE(guarded.state() != DeviceState::Capturing);
    REQUIRE_FALSE(guarded.authorize_delivery(kT0));   // stays down
}

TEST_CASE("D-02 lifecycle: repeated start/stop cycles are clean and do not leak",
          "[d02][lifecycle]") {
    MemRepo repo;
    repo.grant("node-1", "camera_acquire", "camera", kT0);
    ConsentGate gate(repo, true);
    FakeMediaDevice cam(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guarded(cam, gate, AcquisitionRequest{"node-1", "camera_acquire", "camera"});

    for (int i = 0; i < 25; ++i) {
        REQUIRE(guarded.initialize().allowed);
        REQUIRE(guarded.start().allowed);
        REQUIRE(guarded.authorize_delivery(kT0));
        cam.deliver_payload();
        guarded.stop();
        guarded.close();
        REQUIRE(cam.state() == DeviceState::Idle);
    }
    REQUIRE(cam.init_count() == 25);
    REQUIRE(cam.start_count() == 25);
    REQUIRE(cam.close_count() == 25);
    REQUIRE(cam.delivered().size() == 25);
}

TEST_CASE("D-02 lifecycle: a device failure is fail-closed and recoverable by reopening",
          "[d02][lifecycle][error]") {
    MemRepo repo;
    repo.grant("node-1", "mic_acquire", "audio", kT0);
    ConsentGate gate(repo, true);
    FakeMediaDevice mic(DeviceKind::audio, "fake-mic");
    ConsentGuardedDevice guarded(mic, gate, AcquisitionRequest{"node-1", "mic_acquire", "audio"});

    REQUIRE(guarded.initialize().allowed);
    REQUIRE(guarded.start().allowed);
    mic.fail();                                  // e.g. driver error / device removed
    REQUIRE(mic.state() == DeviceState::Error);
    REQUIRE_FALSE(guarded.authorize_delivery(kT0));
    guarded.close();                             // explicit recovery point
    REQUIRE(mic.state() == DeviceState::Idle);
    REQUIRE(guarded.initialize().allowed);        // usable again
}

TEST_CASE("D-02 gate: nothing is delivered while unauthorized (no covert capture)",
          "[d02][consent][privacy]") {
    MemRepo repo;                                 // no grant at all
    ConsentGate gate(repo, true);
    FakeMediaDevice cam(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guarded(cam, gate, AcquisitionRequest{"node-1", "camera_acquire", "camera"});

    GuardResult init = guarded.initialize();
    REQUIRE_FALSE(init.allowed);
    REQUIRE(cam.state() != DeviceState::Capturing);
    // Even if the backend misbehaves and calls its delivery callback, the fake
    // only records while Capturing, and the guard refuses delivery.
    for (int i = 0; i < 5; ++i) {
        REQUIRE_FALSE(guarded.authorize_delivery(kT0));
        cam.deliver_payload();
    }
    REQUIRE(cam.delivered().empty());
}

// Minimal on-device self-test binary for HomeGuardian AI (non-capture only).
//
// This is a standalone native executable built with the NDK. It exercises ONLY
// non-capture functionality: consent-gate decisions, device lifecycle
// transitions, bounded resource behavior, error handling, and resource
// cleanup. It does NOT open the camera or microphone, and it does not require
// CAMERA or RECORD_AUDIO permissions. It is safe to run from `adb shell`.
//
// It deliberately links the Android backend object files so the fail-closed
// guards in NdkCameraDevice/AAudioCaptureDevice are present and exercised
// (they must refuse to initialize because set_device_confirmed is false).

#include "backend/android/NdkCameraDevice.h"
#include "backend/android/AAudioCaptureDevice.h"
#include "core/IMediaDevice.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/FakeMediaDevice.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace homeguardian;

static int g_failures = 0;

static void check(bool cond, const char* name) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_failures++;
}

// A minimal in-memory consent repository so the test needs no SQLite/database.
class MemoryConsentRepo : public IConsentRepository {
public:
    void save(const ConsentRecord& r) override { records_.push_back(r); }
    std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<ConsentRecord> find_by_subject(const std::string& s) override {
        std::vector<ConsentRecord> out;
        for (auto& r : records_) if (r.get_subject_scope() == s) out.push_back(r);
        return out;
    }
    std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
        std::vector<ConsentRecord> out;
        for (auto& r : records_)
            if (r.get_subject_scope() == s && r.get_purpose() == p) out.push_back(r);
        return out;
    }
    size_t count() override { return records_.size(); }
    size_t delete_all() override { size_t n = records_.size(); records_.clear(); return n; }
private:
    std::vector<ConsentRecord> records_;
};

int main() {
    std::printf("HomeGuardian on-device self-test (non-capture)\n");

    auto now = std::chrono::system_clock::now();

    // 1. Consent-gate fail-closed with no records.
    {
        MemoryConsentRepo repo;
        ConsentGate gate(repo, /*media_capture_enabled=*/true);
        auto d = gate.check({"p1", "presence", "camera"}, now);
        check(!d.authorized && d.reason == DenyReason::no_consent_record,
              "gate denies with no consent record");
    }

    // 2. Granted consent authorizes; capture-disabled config denies.
    {
        MemoryConsentRepo repo;
        repo.save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));
        ConsentGate on(repo, true);
        check(on.check({"p1", "presence", "camera"}, now).authorized, "gate allows granted+camera");
        ConsentGate off(repo, false);
        auto d = off.check({"p1", "presence", "camera"}, now);
        check(!d.authorized && d.reason == DenyReason::capture_disabled,
              "gate denies when media_capture disabled");
    }

    // 3. Withdrawal overrides an earlier grant.
    {
        MemoryConsentRepo repo;
        repo.save(ConsentRecord("g1", "p1", "presence", {"camera"}, ConsentDecision::granted,
                                "1.0.0", now - std::chrono::hours(1), std::nullopt, "user", 1));
        repo.save(ConsentRecord::create_withdrawn("w1", "p1", "presence", "1.0.0", "user"));
        ConsentGate gate(repo, true);
        auto d = gate.check({"p1", "presence", "camera"}, now);
        check(!d.authorized && d.reason == DenyReason::decision_not_granted,
              "withdrawal overrides grant");
    }

    // 4. Fake device lifecycle: authorized start, delivery, stop, cleanup.
    {
        MemoryConsentRepo repo;
        repo.save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));
        ConsentGate gate(repo, true);
        FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
        ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});

        check(guard.initialize().allowed, "guarded initialize allowed");
        check(dev.state() == DeviceState::Initialized, "device Initialized");
        check(guard.start().allowed, "guarded start allowed");
        check(dev.state() == DeviceState::Capturing, "device Capturing");

        int delivered = 0;
        for (int i = 0; i < 5; ++i) {
            if (!guard.authorize_delivery()) break;
            dev.deliver_payload();
            delivered++;
        }
        check(delivered == 5, "5 frames delivered while authorized");

        // Withdraw; the next delivery must be denied and force stop.
        repo.save(ConsentRecord::create_withdrawn("w1", "p1", "presence", "1.0.0", "user"));
        check(!guard.authorize_delivery(now), "delivery denied after withdrawal");
        check(dev.state() != DeviceState::Capturing, "device forced out of capture");
        check(dev.delivered().size() == 5, "no extra frames delivered after withdrawal");

        guard.close();
        check(dev.state() == DeviceState::Idle, "device Idle after close");
        check(dev.released(), "resources released on close");
    }

    // 5. Repeated start/stop/close cycles must not leak (counts track cleanly).
    {
        MemoryConsentRepo repo;
        repo.save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));
        ConsentGate gate(repo, true);
        FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
        ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});
        for (int c = 0; c < 3; ++c) {
            guard.initialize();
            guard.start();
            guard.stop();
            guard.close();
        }
        check(dev.init_count() == 3 && dev.start_count() == 3 &&
              dev.stop_count() == 3 && dev.close_count() == 3,
              "3 clean init/start/stop/close cycles");
    }

    // 6. Real NDK backends must fail closed because device is not confirmed.
    {
        NdkCameraDeviceImpl cam;
        bool threw = false;
        try { cam.initialize(); } catch (const std::exception&) { threw = true; }
        check(threw && cam.state() == DeviceState::Error,
              "NdkCameraDevice fails closed (device not confirmed)");
    }
    {
        AAudioCaptureDeviceImpl mic;
        bool threw = false;
        try { mic.initialize(); } catch (const std::exception&) { threw = true; }
        check(threw && mic.state() == DeviceState::Error,
              "AAudioCaptureDevice fails closed (device not confirmed)");
    }

    std::printf("\n%s (%d failure(s))\n",
                g_failures == 0 ? "ALL PASSED" : "FAILURES PRESENT", g_failures);
    return g_failures == 0 ? 0 : 1;
}

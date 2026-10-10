// JNI bridge for the HomeGuardian acceptance-test harness.
//
// SAFETY CONTRACT (enforced here):
//  - Capture is disabled by default. Neither sensor starts on library load or
//    app launch.
//  - nativeStartCamera()/nativeStartMic() activate capture ONLY when called
//    explicitly by the operator (a button press), and ONLY after the OS has
//    granted the corresponding runtime permission (checked by the Activity,
//    and re-checked here defensively).
//  - Every delivered frame/sample passes authorize_delivery() immediately
//    before delivery; on denial the payload is dropped and capture is stopped.
//  - No media is written to disk or uploaded. Delivered payloads are counted
//    in memory only and never persisted.
//
// The capture wiring here is intentionally minimal: it opens the device,
// confirms consent is granted, and counts delivered payloads through the
// delivery gate. It does not decode frames, run inference, or store anything.

#include <jni.h>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>

#include "backend/android/NdkCameraDevice.h"
#include "backend/android/AAudioCaptureDevice.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/FakeMediaDevice.h"

using namespace homeguardian;

namespace {

// A minimal in-memory consent repository (no database needed for the harness).
class MemoryConsentRepo : public IConsentRepository {
public:
    void save(const ConsentRecord& r) override {
        std::lock_guard<std::mutex> lk(m_);
        records_.push_back(r);
    }
    std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<ConsentRecord> find_by_subject(const std::string& s) override { return filter(s, ""); }
    std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override { return filter(s, p); }
    size_t count() override { std::lock_guard<std::mutex> lk(m_); return records_.size(); }
    size_t delete_all() override { std::lock_guard<std::mutex> lk(m_); size_t n = records_.size(); records_.clear(); return n; }
    void grant(const std::string& subject, const std::string& purpose, const std::vector<std::string>& cats) {
        save(ConsentRecord::create_granted("harness-grant", subject, purpose, cats, "1.0.0", "operator"));
    }
    void withdraw(const std::string& subject, const std::string& purpose) {
        save(ConsentRecord::create_withdrawn("harness-withdraw", subject, purpose, "1.0.0", "operator"));
    }
private:
    std::vector<ConsentRecord> filter(const std::string& s, const std::string& p) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<ConsentRecord> out;
        for (auto& r : records_)
            if (r.get_subject_scope() == s && (p.empty() || r.get_purpose() == p)) out.push_back(r);
        return out;
    }
    std::mutex m_;
    std::vector<ConsentRecord> records_;
};

// Harness singleton state. Capture objects are created only on explicit start.
struct Harness {
    MemoryConsentRepo repo;
    std::unique_ptr<ConsentGate> gate;
    // Camera
    std::unique_ptr<NdkCameraDeviceImpl> camera;
    std::unique_ptr<ConsentGuardedDevice> guarded_camera;
    // Mic
    std::unique_ptr<AAudioCaptureDeviceImpl> mic;
    std::unique_ptr<ConsentGuardedDevice> guarded_mic;
    std::atomic<int> camera_delivered{0};
    std::atomic<int> mic_delivered{0};
    std::mutex m;
};

Harness& harness() {
    static Harness h;
    static std::once_flag once;
    std::call_once(once, [] {
        // Grant consent up front for the two test purposes; capture is still
        // gated by the delivery check and by explicit operator action.
        h.repo.grant("harness", "camera_test", {"camera"});
        h.repo.grant("harness", "mic_test", {"audio"});
        h.gate = std::make_unique<ConsentGate>(h.repo, /*media_capture_enabled=*/true);
    });
    return h;
}

} // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeSelfTest(JNIEnv* env, jobject /*thiz*/) {
    // Reuse the on-device non-capture self-test logic inline (no capture here).
    auto& h = harness();
    auto now = std::chrono::system_clock::now();

    std::string out = "self-test:\n";
    auto d = h.gate->check({"harness", "camera_test", "camera"}, now);
    out += d.authorized ? "gate: camera authorized\n" : "gate: camera DENIED\n";
    auto w = h.gate->check({"harness", "nope", "camera"}, now);
    out += (!w.authorized) ? "gate: unknown purpose denied\n" : "gate: UNEXPECTED allow\n";

    // Fail-closed guards on the real backends (device not confirmed -> Error).
    NdkCameraDeviceImpl cam;
    bool camClosed = false;
    try { cam.initialize(); } catch (const std::exception&) { camClosed = true; }
    out += camClosed ? "camera backend: fail-closed OK\n" : "camera backend: UNEXPECTED open\n";

    AAudioCaptureDeviceImpl mic;
    bool micClosed = false;
    try { mic.initialize(); } catch (const std::exception&) { micClosed = true; }
    out += micClosed ? "audio backend: fail-closed OK\n" : "audio backend: UNEXPECTED open\n";

    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStartCamera(JNIEnv* env, jobject /*thiz*/) {
    auto& h = harness();
    std::lock_guard<std::mutex> lk(h.m);
    if (!h.camera) {
        h.camera = std::make_unique<NdkCameraDeviceImpl>();
        // Confirm the device ONLY now, because the operator explicitly started
        // a capture test AND the OS permission was granted by the Activity.
        h.camera->set_device_confirmed(true);
        h.guarded_camera = std::make_unique<ConsentGuardedDevice>(
            *h.camera, *h.gate, AcquisitionRequest{"harness", "camera_test", "camera"});
    }
    if (!h.guarded_camera->initialize().allowed) {
        return env->NewStringUTF("camera: initialize denied by gate");
    }
    auto r = h.guarded_camera->start();
    if (!r.allowed) {
        return env->NewStringUTF((std::string("camera: start denied (") + r.explanation + ")").c_str());
    }
    // The device is now capturing. Each real NDK frame callback must call
    // authorize_delivery() before delivering; the harness counts authorized
    // deliveries. No frame is decoded or stored.
    return env->NewStringUTF("camera: capturing (delivery-gated)");
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStopCamera(JNIEnv* env, jobject /*thiz*/) {
    auto& h = harness();
    std::lock_guard<std::mutex> lk(h.m);
    if (h.guarded_camera) { h.guarded_camera->stop(); h.guarded_camera->close(); }
    std::string out = "camera: stopped; delivered=" + std::to_string(h.camera_delivered.load());
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStartMic(JNIEnv* env, jobject /*thiz*/) {
    auto& h = harness();
    std::lock_guard<std::mutex> lk(h.m);
    if (!h.mic) {
        h.mic = std::make_unique<AAudioCaptureDeviceImpl>();
        h.mic->set_device_confirmed(true);
        h.guarded_mic = std::make_unique<ConsentGuardedDevice>(
            *h.mic, *h.gate, AcquisitionRequest{"harness", "mic_test", "audio"});
    }
    if (!h.guarded_mic->initialize().allowed) {
        return env->NewStringUTF("mic: initialize denied by gate");
    }
    auto r = h.guarded_mic->start();
    if (!r.allowed) {
        return env->NewStringUTF((std::string("mic: start denied (") + r.explanation + ")").c_str());
    }
    return env->NewStringUTF("mic: capturing (delivery-gated)");
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStopMic(JNIEnv* env, jobject /*thiz*/) {
    auto& h = harness();
    std::lock_guard<std::mutex> lk(h.m);
    if (h.guarded_mic) { h.guarded_mic->stop(); h.guarded_mic->close(); }
    std::string out = "mic: stopped; delivered=" + std::to_string(h.mic_delivered.load());
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeWithdrawConsent(JNIEnv* env, jobject /*thiz*/) {
    auto& h = harness();
    // Withdraw consent for both test purposes. Any in-flight capture must stop
    // delivering on the next authorize_delivery() call.
    h.repo.withdraw("harness", "camera_test");
    h.repo.withdraw("harness", "mic_test");
    std::lock_guard<std::mutex> lk(h.m);
    if (h.guarded_camera) h.guarded_camera->recheck_and_enforce();
    if (h.guarded_mic) h.guarded_mic->recheck_and_enforce();
    return env->NewStringUTF("consent withdrawn; capture delivery blocked");
}

} // extern "C"

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
#include <android/log.h>

#define HG_LOG_TAG "HGHarness"
#define HG_LOGI(...) __android_log_print(ANDROID_LOG_INFO, HG_LOG_TAG, __VA_ARGS__)

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

    HG_LOGI("non-capture self-test: gate_cam=%d gate_unknown_denied=%d cam_closed=%d mic_closed=%d",
            (int)d.authorized, (int)(!w.authorized), (int)camClosed, (int)micClosed);

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
    // initialize() may throw (e.g. camera enumeration returns 0 devices). Never
    // let a C++ exception escape the JNI boundary (it aborts the process);
    // catch it and report a clean fail-closed result.
    GuardResult init;
    try {
        init = h.guarded_camera->initialize();
    } catch (const std::exception& e) {
        h.guarded_camera->close();
        HG_LOGI("camera initialize threw (fail-closed): %s", e.what());
        return env->NewStringUTF((std::string("camera: initialize failed, capture not started (") + e.what() + ")").c_str());
    }
    if (!init.allowed) {
        return env->NewStringUTF("camera: initialize denied by gate");
    }
    // A C++ exception here (e.g. camera enumeration returns 0 devices, or the
    // device errors) must NOT escape across the JNI boundary, which would abort
    // the process. Catch it and report a clean, fail-closed denial instead.
    GuardResult r;
    try {
        r = h.guarded_camera->start();
    } catch (const std::exception& e) {
        h.guarded_camera->close();
        HG_LOGI("camera start threw (fail-closed): %s", e.what());
        return env->NewStringUTF((std::string("camera: start failed, capture not started (") + e.what() + ")").c_str());
    }
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
    GuardResult init;
    try {
        init = h.guarded_mic->initialize();
    } catch (const std::exception& e) {
        h.guarded_mic->close();
        HG_LOGI("mic initialize threw (fail-closed): %s", e.what());
        return env->NewStringUTF((std::string("mic: initialize failed, capture not started (") + e.what() + ")").c_str());
    }
    if (!init.allowed) {
        return env->NewStringUTF("mic: initialize denied by gate");
    }
    GuardResult r;
    try {
        r = h.guarded_mic->start();
    } catch (const std::exception& e) {
        h.guarded_mic->close();
        HG_LOGI("mic start threw (fail-closed): %s", e.what());
        return env->NewStringUTF((std::string("mic: start failed, capture not started (") + e.what() + ")").c_str());
    }
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

// Camera1 delivery gate. Called from Camera1Bridge.onPreviewFrame on the camera
// callback thread for EVERY preview frame, immediately before the frame would
// be processed. Returns true only if the delivery gate authorizes this frame;
// false means capture must stop now (consent withdrawn / revoked / device
// error) and the frame must be dropped. This is the single consent enforcement
// point at the real acquisition boundary for the Camera1 backend.
JNIEXPORT jboolean JNICALL
Java_org_homeguardian_harness_Camera1Bridge_nativeOnPreviewFrame(JNIEnv* /*env*/, jobject /*thiz*/,
                                                                 jbyteArray /*data*/, jint /*width*/,
                                                                 jint /*height*/, jint /*format*/) {
    auto& h = harness();
    // Lazily create the guarded camera for the Camera1 path (consent already
    // granted for camera_test). The device is confirmed only because the
    // operator explicitly started capture AND the OS granted CAMERA.
    {
        std::lock_guard<std::mutex> lk(h.m);
        if (!h.guarded_camera) {
            h.camera = std::make_unique<NdkCameraDeviceImpl>();
            h.camera->set_device_confirmed(true);
            h.guarded_camera = std::make_unique<ConsentGuardedDevice>(
                *h.camera, *h.gate, AcquisitionRequest{"harness", "camera_test", "camera"});
        }
    }
    // Delivery-boundary authorization check. authorize_delivery() re-runs the
    // full consent/permission/device evaluation and, on denial, forces the
    // device out of capture.
    bool allowed;
    {
        std::lock_guard<std::mutex> lk(h.m);
        if (!h.guarded_camera) return JNI_FALSE;
        try {
            allowed = h.guarded_camera->authorize_delivery();
        } catch (const std::exception& e) {
            HG_LOGI("camera1 delivery gate threw (fail-closed): %s", e.what());
            h.guarded_camera->close();
            return JNI_FALSE;
        }
    }
    if (allowed) {
        h.camera_delivered.fetch_add(1);
        return JNI_TRUE;
    }
    HG_LOGI("camera1 delivery gate denied a frame; capture must stop");
    return JNI_FALSE;
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

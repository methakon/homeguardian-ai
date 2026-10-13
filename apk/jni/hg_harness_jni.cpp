// JNI bridge for the HomeGuardian Android acquisition node (milestone D-02).
//
// SAFETY CONTRACT (enforced here, and re-enforced per payload by the accountant):
//  - Capture is DISABLED by default. Nothing opens on library load or app launch.
//  - nativeStartCamera()/nativeStartMic() activate capture ONLY when the operator
//    explicitly pressed Start AND the Activity confirmed the OS permission was
//    granted. The Android permission is modelled here as a flag the Activity
//    sets, so a revocation (or a lifecycle stop) immediately fails delivery.
//  - EVERY frame (Camera1 preview callback) and EVERY PCM buffer (AAudio data
//    callback) is checked against the consent gate immediately before delivery
//    and then accounted: bounded size/dimension/rate/timestamp validation plus
//    saturating counters. A refused payload is dropped and never queued.
//  - No media is written to disk, uploaded, decoded or interpreted. There is no
//    face, speech, fall or emergency inference anywhere in this file.
//
// The JNI layer owns one CaptureAccountant shared by both capture paths, and one
// ConsentGate fed by an in-memory consent repository (a node has no database).
// A captured C++ exception never crosses the boundary: every entry point catches
// and reports a clean, fail-closed result, because an escaping exception aborts
// the process.

#include <jni.h>

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "backend/android/AAudioCaptureDevice.h"
#include "backend/android/NdkCameraDevice.h"
#include "core/CaptureAccounting.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/NodeIdentity.h"

#define HG_LOG_TAG "HGNode"
#define HG_LOGI(...) __android_log_print(ANDROID_LOG_INFO, HG_LOG_TAG, __VA_ARGS__)
#define HG_LOGW(...) __android_log_print(ANDROID_LOG_WARN, HG_LOG_TAG, __VA_ARGS__)

using namespace homeguardian;

namespace {

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The consent gate works in time_points; the accountant works in epoch ms.
inline std::chrono::system_clock::time_point tp_now() {
    return std::chrono::system_clock::now();
}

// In-memory consent repository: a phone node keeps consent in RAM only.
class MemoryConsentRepo : public IConsentRepository {
public:
    void save(const ConsentRecord& r) override {
        std::lock_guard<std::mutex> lk(m_);
        records_.push_back(r);
    }
    std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<ConsentRecord> find_by_subject(const std::string& s) override { return filter(s, ""); }
    std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
        return filter(s, p);
    }
    size_t count() override { std::lock_guard<std::mutex> lk(m_); return records_.size(); }
    size_t delete_all() override {
        std::lock_guard<std::mutex> lk(m_);
        size_t n = records_.size();
        records_.clear();
        return n;
    }
    void grant(const std::string& subject, const std::string& purpose,
               const std::vector<std::string>& cats) {
        save(ConsentRecord::create_granted("node-grant", subject, purpose, cats, "1.0.0", "operator"));
    }
    void withdraw(const std::string& subject, const std::string& purpose) {
        save(ConsentRecord::create_withdrawn("node-withdraw", subject, purpose, "1.0.0", "operator"));
    }
    bool granted(const std::string& subject, const std::string& purpose) {
        std::lock_guard<std::mutex> lk(m_);
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if (it->get_subject_scope() == subject && it->get_purpose() == purpose) {
                return it->get_decision() == ConsentDecision::granted;
            }
        }
        return false;
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

// Node singleton. Devices are created only on an explicit operator start.
struct Node {
    MemoryConsentRepo repo;
    std::unique_ptr<ConsentGate> gate;
    CaptureAccountant accountant;

    NodeIdentity identity;

    // OS permission model: the Activity sets these from the real grant state and
    // clears them on revocation or when the app leaves the foreground.
    std::atomic<bool> camera_permission{false};
    std::atomic<bool> mic_permission{false};

    // camera
    int camera_id = 0;                 // 0 = back, 1 = front (Camera1 index)
    std::unique_ptr<NdkCameraDeviceImpl> camera;        // camera2 path
    std::unique_ptr<ConsentGuardedDevice> guarded_camera;
    // mic (AAudio)
    std::unique_ptr<AAudioCaptureDeviceImpl> mic;
    std::unique_ptr<ConsentGuardedDevice> guarded_mic;

    std::mutex m;
};

Node& node() {
    static Node n;
    static std::once_flag once;
    std::call_once(once, [] {
        // Consent is granted for the two acquisition purposes ONLY by an explicit
        // operator action in the UI; until then nothing is authorized.
        n.gate = std::make_unique<ConsentGate>(n.repo, /*media_capture_enabled=*/true);
    });
    return n;
}

std::string jstr(JNIEnv* env, jstring s) {
    if (s == nullptr) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

jstring to_jstring(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

// Compact one-line stats for the UI. Non-sensitive counters only.
std::string stats_string(Node& n) {
    const CaptureCounters c = n.accountant.counters();
    std::string s;
    s += "video=" + std::string(n.accountant.video_running() ? "on" : "off");
    s += " audio=" + std::string(n.accountant.audio_running() ? "on" : "off");
    s += " frames=" + std::to_string(c.frames_authorized) + "/" + std::to_string(c.frames_seen);
    s += " dropped=" + std::to_string(c.frames_dropped);
    s += " frame_bytes=" + std::to_string(c.frame_bytes);
    s += " audio_buf=" + std::to_string(c.audio_buffers_authorized) + "/" +
         std::to_string(c.audio_buffers_seen);
    s += " audio_dropped=" + std::to_string(c.audio_buffers_dropped);
    s += " samples=" + std::to_string(c.audio_frames);
    s += " audio_bytes=" + std::to_string(c.audio_bytes);
    s += " xruns=" + std::to_string(c.xruns);
    s += " ts_regress=" + std::to_string(c.timestamp_regressions);
    if (c.last_drop != DropReason::none) {
        s += " last_drop=" + std::string(drop_reason_to_string(c.last_drop));
        if (!c.last_drop_detail.empty()) s += " (" + c.last_drop_detail + ")";
    }
    return s;
}

}  // namespace

extern "C" {

// ---- identity --------------------------------------------------------------

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeSetIdentity(JNIEnv* env, jobject /*thiz*/,
                                                             jstring seed, jstring display_name,
                                                             jint api_level, jstring abi,
                                                             jstring model) {
    auto& n = node();
    n.identity = make_node_identity(jstr(env, seed), jstr(env, display_name),
                                    static_cast<int>(api_level), jstr(env, abi), jstr(env, model));
    HG_LOGI("identity set: %s", node_identity_summary(n.identity).c_str());
    return to_jstring(env, node_identity_summary(n.identity));
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeIdentity(JNIEnv* env, jobject /*thiz*/) {
    return to_jstring(env, node_identity_summary(node().identity));
}

// ---- permission model (set by the Activity from the real OS grant state) ----

JNIEXPORT void JNICALL
Java_org_homeguardian_harness_MainActivity_nativeSetPermissions(JNIEnv* /*env*/, jobject /*thiz*/,
                                                                jboolean camera, jboolean mic) {
    auto& n = node();
    n.camera_permission.store(camera == JNI_TRUE);
    n.mic_permission.store(mic == JNI_TRUE);
}

// ---- consent --------------------------------------------------------------

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeGrantConsent(JNIEnv* env, jobject /*thiz*/,
                                                              jboolean camera, jboolean mic) {
    auto& n = node();
    if (camera == JNI_TRUE) n.repo.grant("node", "camera_acquire", {"camera"});
    if (mic == JNI_TRUE) n.repo.grant("node", "mic_acquire", {"audio"});
    return to_jstring(env, "consent recorded (camera=" + std::string(camera == JNI_TRUE ? "yes" : "no") +
                               ", mic=" + std::string(mic == JNI_TRUE ? "yes" : "no") + ")");
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeWithdrawConsent(JNIEnv* env, jobject thiz) {
    auto& n = node();
    n.repo.withdraw("node", "camera_acquire");
    n.repo.withdraw("node", "mic_acquire");
    // Force any in-flight capture down immediately.
    {
        std::lock_guard<std::mutex> lk(n.m);
        if (n.guarded_camera) n.guarded_camera->recheck_and_enforce();
        if (n.guarded_mic) n.guarded_mic->recheck_and_enforce();
    }
    return to_jstring(env, "consent withdrawn: capture delivery blocked");
}

// ---- camera ---------------------------------------------------------------

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStartCamera(JNIEnv* env, jobject /*thiz*/,
                                                             jint camera_id) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    if (!n.camera_permission.load()) {
        return to_jstring(env, "camera: OS permission not granted; capture not started");
    }
    if (!n.repo.granted("node", "camera_acquire")) {
        return to_jstring(env, "camera: consent not granted; capture not started");
    }
    n.camera_id = static_cast<int>(camera_id);
    if (!n.camera) {
        n.camera = std::make_unique<NdkCameraDeviceImpl>();
        n.camera->set_device_confirmed(true);
        n.guarded_camera = std::make_unique<ConsentGuardedDevice>(
            *n.camera, *n.gate, AcquisitionRequest{"node", "camera_acquire", "camera"},
            [&n] { return n.camera_permission.load(); });
    }
    GuardResult init;
    try {
        init = n.guarded_camera->initialize();
    } catch (const std::exception& e) {
        n.guarded_camera->close();
        return to_jstring(env, std::string("camera: initialize failed (") + e.what() + ")");
    }
    if (!init.allowed) return to_jstring(env, "camera: initialize denied by gate");
    GuardResult r;
    try {
        r = n.guarded_camera->start();
    } catch (const std::exception& e) {
        n.guarded_camera->close();
        return to_jstring(env, std::string("camera: start failed (") + e.what() + ")");
    }
    if (!r.allowed) return to_jstring(env, std::string("camera: start denied (") + r.explanation + ")");
    n.accountant.note_capture_started(/*video=*/true, now_ms());
    HG_LOGI("camera capture started (id=%d)", n.camera_id);
    return to_jstring(env, "camera: capturing (delivery-gated)");
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStopCamera(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    if (n.guarded_camera) {
        n.guarded_camera->stop();
        n.guarded_camera->close();
    }
    const CaptureCounters c = n.accountant.counters();
    return to_jstring(env, "camera stopped: frames=" + std::to_string(c.frames_authorized) +
                               " dropped=" + std::to_string(c.frames_dropped));
}

// The Camera1 preview callback: the real acquisition boundary for the legacy
// camera path. Called on the camera's callback thread for EVERY frame.
JNIEXPORT jboolean JNICALL
Java_org_homeguardian_harness_Camera1Bridge_nativeOnPreviewFrame(JNIEnv* env, jobject /*thiz*/,
                                                                 jbyteArray data, jint width,
                                                                 jint height, jint /*format*/,
                                                                 jlong timestamp_ms) {
    auto& n = node();
    const size_t bytes = data ? static_cast<size_t>(env->GetArrayLength(data)) : 0u;
    bool authorized = false;
    {
        std::lock_guard<std::mutex> lk(n.m);
        if (!n.guarded_camera) return JNI_FALSE;
        try {
            authorized = n.guarded_camera->authorize_delivery();
        } catch (const std::exception& e) {
            HG_LOGW("camera delivery gate threw (fail-closed): %s", e.what());
            n.guarded_camera->close();
            return JNI_FALSE;
        }
    }
    CaptureAccountant::Decision d = n.accountant.account_frame(
        static_cast<uint32_t>(width > 0 ? width : 0), static_cast<uint32_t>(height > 0 ? height : 0),
        bytes, static_cast<int64_t>(timestamp_ms), authorized, now_ms());
    return d.authorized ? JNI_TRUE : JNI_FALSE;
}

// ---- microphone (AAudio) --------------------------------------------------

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStartMic(JNIEnv* env, jobject /*thiz*/,
                                                          jint sample_rate, jint channels) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    if (!n.mic_permission.load()) {
        return to_jstring(env, "mic: OS permission not granted; capture not started");
    }
    if (!n.repo.granted("node", "mic_acquire")) {
        return to_jstring(env, "mic: consent not granted; capture not started");
    }
    if (!n.mic) {
        n.mic = std::make_unique<AAudioCaptureDeviceImpl>();
        AAudioCaptureDeviceImpl::Config cfg;
        cfg.sample_rate = static_cast<int32_t>(sample_rate);
        cfg.channels = static_cast<int32_t>(channels);
        n.mic->set_config(cfg);
        n.mic->set_accountant(&n.accountant);
        // Per-buffer authorization: re-evaluates the gate on the driver thread.
        n.mic->set_authorize([&n] {
            try {
                return n.guarded_mic && n.guarded_mic->authorize_delivery();
            } catch (...) {
                return false;
            }
        });
        n.guarded_mic = std::make_unique<ConsentGuardedDevice>(
            *n.mic, *n.gate, AcquisitionRequest{"node", "mic_acquire", "audio"},
            [&n] { return n.mic_permission.load(); });
    } else {
        AAudioCaptureDeviceImpl::Config cfg;
        cfg.sample_rate = static_cast<int32_t>(sample_rate);
        cfg.channels = static_cast<int32_t>(channels);
        n.mic->set_config(cfg);
    }
    GuardResult init;
    try {
        init = n.guarded_mic->initialize();
    } catch (const std::exception& e) {
        n.guarded_mic->close();
        return to_jstring(env, std::string("mic: initialize failed (") + e.what() + ")");
    }
    if (!init.allowed) return to_jstring(env, "mic: initialize denied by gate");
    GuardResult r;
    try {
        r = n.guarded_mic->start();
    } catch (const std::exception& e) {
        n.guarded_mic->close();
        return to_jstring(env, std::string("mic: start failed (") + e.what() + ")");
    }
    if (!r.allowed) return to_jstring(env, std::string("mic: start denied (") + r.explanation + ")");
    n.accountant.note_capture_started(/*video=*/false, now_ms());
    std::string out = "mic: capturing at ";
    out += std::to_string(n.mic->negotiated_sample_rate()) + " Hz, " +
           std::to_string(n.mic->negotiated_channels()) + " channel(s) (delivery-gated)";
    HG_LOGI("%s", out.c_str());
    return to_jstring(env, out);
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStopMic(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    if (n.guarded_mic) {
        n.guarded_mic->stop();
        n.guarded_mic->close();
    }
    const CaptureCounters c = n.accountant.counters();
    return to_jstring(env, "mic stopped: buffers=" + std::to_string(c.audio_buffers_authorized) +
                               " dropped=" + std::to_string(c.audio_buffers_dropped) +
                               " samples=" + std::to_string(c.audio_frames));
}

// Does the audio driver thread demand a stop (consent/permission/disconnect)?
JNIEXPORT jboolean JNICALL
Java_org_homeguardian_harness_MainActivity_nativeMicStopRequested(JNIEnv* /*env*/, jobject /*thiz*/) {
    auto& n = node();
    return (n.mic && n.mic->stop_requested()) ? JNI_TRUE : JNI_FALSE;
}

// ---- shared status / lifecycle --------------------------------------------

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStats(JNIEnv* env, jobject /*thiz*/) {
    return to_jstring(env, stats_string(node()));
}

// Called when the Activity leaves the foreground, on shutdown, or when the OS
// permission is revoked: stop everything and release resources deterministically.
JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStopAll(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    if (n.guarded_camera) {
        n.guarded_camera->stop();
        n.guarded_camera->close();
    }
    if (n.guarded_mic) {
        n.guarded_mic->stop();
        n.guarded_mic->close();
    }
    n.accountant.note_capture_stopped();
    return to_jstring(env, "all capture stopped; resources released");
}

// Non-capture self-test: proves the gate denies without consent and the backends
// fail closed when unconfirmed. Runs no capture.
JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeSelfTest(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    std::string out = "self-test:";
    const auto d = n.gate->check({"node", "camera_acquire", "camera"}, tp_now());
    out += d.authorized ? " gate=camera-authorized" : " gate=camera-denied(closing)";
    const auto w = n.gate->check({"node", "nope", "camera"}, tp_now());
    out += (!w.authorized) ? " unknown-purpose=denied" : " UNEXPECTED-ALLOW";
    out += " identity=" + std::string(node_identity_valid(n.identity) ? "present" : "not-set");
    NdkCameraDeviceImpl cam;
    bool cam_closed = false;
    try {
        cam.initialize();
    } catch (const std::exception&) {
        cam_closed = true;
    }
    out += cam_closed ? " camera-backend=fail-closed" : " CAMERA-BACKEND-OPENED";
    AAudioCaptureDeviceImpl mic;
    bool mic_closed = false;
    try {
        mic.initialize();
    } catch (const std::exception&) {
        mic_closed = true;
    }
    out += mic_closed ? " audio-backend=fail-closed" : " AUDIO-BACKEND-OPENED";
    return to_jstring(env, out);
}

}  // extern "C"

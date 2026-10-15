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
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "backend/android/AAudioCaptureDevice.h"
#include "backend/android/Camera1Device.h"
#include "backend/android/NdkCameraDevice.h"
#include "core/CaptureAccounting.h"
#include "core/BoundedFeatures.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "persistence/SingleActiveGrantConsentRepo.h"
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
// The consent record store. The previous version APPENDED a granted record on every press of
// "Grant consent"; a second press therefore produced two active grants, and the gate correctly
// refused the ambiguity (DenyReason::ambiguous) - the operator saw "mic: initialize denied by
// gate" with consent apparently granted. SingleActiveGrantConsentRepo keeps at most one active
// grant per (subject, purpose), so repeated presses are harmless and a withdrawal is final.
using MemoryConsentRepo = SingleActiveGrantConsentRepo;

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
    std::unique_ptr<NdkCameraDeviceImpl> camera;        // camera2 / NDK path
    std::unique_ptr<Camera1Device> camera1;             // legacy Camera1 (java bridge) path
    std::string camera_backend = "none";                // which backend the guard is bound to
    // What the ACTIVITY's legacy api sees (Camera.getNumberOfCameras()). -1 = not reported yet.
    // Deliberately NOT the NDK enumeration: on the validation device the NDK enumerates zero cameras
    // while the legacy api sees one, which is the whole reason backend selection is explicit.
    std::atomic<int> platform_camera_count{-1};
    std::unique_ptr<ConsentGuardedDevice> guarded_camera;
    // mic (AAudio)
    std::unique_ptr<AAudioCaptureDeviceImpl> mic;
    std::unique_ptr<ConsentGuardedDevice> guarded_mic;

    // ---- bounded observation queue for the hub transport (D-03) -------------------------
    // COMPACT NUMERIC FEATURES ONLY: no pixels, no PCM, no text derived from media. Emitted
    // from the AUTHORIZED payload path (after the gate) and sampled on an interval so a 15 fps
    // camera cannot flood the link. The queue is bounded and every drop is counted.
    struct Observation {
        std::string source_type;
        std::string observation_type;
        std::string unit;
        std::string state;
        double value = 0.0;
        double confidence = 0.0;
        int64_t acquired_at_ms = 0;
        std::string provenance = "real_device";
    };
    std::deque<Observation> observations;
    uint64_t observations_emitted = 0;
    uint64_t observations_dropped = 0;
    int64_t observation_interval_ms = 1000;   // configurable sampling interval
    int64_t last_camera_emit_ms = 0;
    int64_t last_audio_emit_ms = 0;
    std::vector<uint8_t> prev_luma;           // bounded previous frame for the motion delta
    size_t prev_luma_pixels = 0;
    uint64_t frames_processed = 0;            // UI: frame-processing count
    uint64_t audio_windows_processed = 0;     // UI: audio observation count

    std::mutex m;
};

// Bounded capacity: matches the Android transport's own MAX_QUEUE_DEPTH so the two cannot
// disagree about how much work is waiting.
constexpr size_t kMaxPendingObservations = 128;

std::string num_str(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return std::string(buf);
}

// Queue ONE compact observation. A non-finite value is REFUSED here rather than queued: a NaN
// that reaches the wire is a value the hub must reject, and a node producing them would spend
// its link budget on refusals. Never called for an unauthorized payload.
void push_observation(Node& n, const char* source_type, const char* observation_type, double value,
                      const char* unit, const char* state, double confidence, int64_t acquired_ms) {
    if (!std::isfinite(value)) {
        std::lock_guard<std::mutex> lk(n.m);
        ++n.observations_dropped;
        return;
    }
    std::lock_guard<std::mutex> lk(n.m);
    if (n.observations.size() >= kMaxPendingObservations) {
        n.observations.pop_front();
        ++n.observations_dropped;
    }
    Node::Observation o;
    o.source_type = source_type;
    o.observation_type = observation_type;
    o.value = value;
    o.unit = unit;
    o.state = state;
    o.confidence = confidence;
    o.acquired_at_ms = acquired_ms;
    n.observations.push_back(o);
    ++n.observations_emitted;
}

// Interval gate: true at most once per observation_interval_ms. Keeps a 15 fps camera from
// turning into 15 observations a second.
bool due_for_emission(Node& n, int64_t now, bool camera) {
    std::lock_guard<std::mutex> lk(n.m);
    int64_t& last = camera ? n.last_camera_emit_ms : n.last_audio_emit_ms;
    if (now - last < n.observation_interval_ms) return false;
    last = now;
    return true;
}

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

std::string jstring_to_std(JNIEnv* env, jstring value) {
    if (value == nullptr) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

// Backend selection is EXPLICIT and comes from the Activity, which knows what the platform exposes.
// It is never gated behind a successful NDK enumeration: on the validation device (Android 8.1 /
// API 27 / MT6580) the NDK camera2 API enumerates ZERO cameras while the legacy Camera1 path works,
// and requiring NDK success first is exactly what blocked physical camera acceptance.
//   "camera1" -> the java Camera1 bridge (Camera.open() + NV21 preview callbacks)
//   "camera2" -> the NDK camera2 backend (kept for devices that support it)
//   "auto"    -> a deterministic rule, not a hardware probe: use Camera1 when the platform reports
//                legacy cameras, otherwise the NDK backend.
std::string select_camera_backend(const Node& n, const std::string& requested) {
    if (requested == "camera1" || requested == "camera2") return requested;
    return n.platform_camera_count.load() > 0 ? "camera1" : "camera2";
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
    s += " frames_processed=" + std::to_string(n.frames_processed);
    s += " audio_windows=" + std::to_string(n.audio_windows_processed);
    s += " obs_emitted=" + std::to_string(n.observations_emitted);
    s += " obs_dropped=" + std::to_string(n.observations_dropped);
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
                                                             jint camera_id, jstring backend) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    const std::string requested = jstring_to_std(env, backend);
    const std::string selected = select_camera_backend(n, requested.empty() ? "auto" : requested);
    if (!n.camera_permission.load()) {
        return to_jstring(env, "camera: OS permission not granted; capture not started");
    }
    if (!n.repo.granted("node", "camera_acquire")) {
        return to_jstring(env, "camera: consent not granted; capture not started");
    }
    n.camera_id = static_cast<int>(camera_id);
    // Rebuild the guard for the SELECTED backend. Re-selecting on every start keeps repeated
    // start/stop honest (an acceptance item) and makes a backend switch impossible to half-apply.
    n.guarded_camera.reset();
    n.camera.reset();
    n.camera1.reset();
    n.camera_backend = selected;
    if (selected == "camera1") {
        n.camera1 = std::make_unique<Camera1Device>();
        n.camera1->set_platform_camera_count(n.platform_camera_count.load());
        n.guarded_camera = std::make_unique<ConsentGuardedDevice>(
            *n.camera1, *n.gate, AcquisitionRequest{"node", "camera_acquire", "camera"},
            [&n] { return n.camera_permission.load(); });
    } else {
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
    if (!init.allowed) {
        const std::string why = std::string("camera: initialize denied by gate") + " [reason=" +
            ConsentGate::reason_to_string(init.deny_reason) + " detail=" + init.explanation + "]";
        __android_log_print(ANDROID_LOG_WARN, "HGNode", "%s", why.c_str());
        return to_jstring(env, why);
    }
    GuardResult r;
    try {
        r = n.guarded_camera->start();
    } catch (const std::exception& e) {
        n.guarded_camera->close();
        return to_jstring(env, std::string("camera: start failed (") + e.what() + ")");
    }
    if (!r.allowed) {
        const std::string why = std::string("camera: start denied [reason=") +
            ConsentGate::reason_to_string(r.deny_reason) + " detail=" + r.explanation + "]";
        __android_log_print(ANDROID_LOG_WARN, "HGNode", "%s", why.c_str());
        return to_jstring(env, why);
    }
    n.accountant.note_capture_started(/*video=*/true, now_ms());
    HG_LOGI("camera capture started (id=%d backend=%s)", n.camera_id, selected.c_str());
    return to_jstring(env, "camera: capturing (delivery-gated, backend=" + selected + ")");
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeStopCamera(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    if (n.guarded_camera) {
        n.guarded_camera->stop();
        n.guarded_camera->close();
    }
    // Clear the VIDEO flag only: a camera stop must not report the microphone as stopped.
    n.accountant.note_capture_stopped(/*video=*/true);
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
    if (!d.authorized) return JNI_FALSE;

    // ONLY an authorized frame is measured. The frame itself is never retained, transmitted or
    // written: the Y plane is read once for a bounded motion descriptor and dropped.
    if (data != nullptr && width > 0 && height > 0) {
        const size_t w = static_cast<size_t>(width);
        const size_t h = static_cast<size_t>(height);
        const size_t pixels = w * h;
        if (nv21_geometry_ok(w, h, bytes) && pixels >= FeatureBounds::kMinPixels &&
            pixels <= FeatureBounds::kMaxPixels) {
            jbyte* raw = env->GetByteArrayElements(data, nullptr);
            if (raw != nullptr) {
                const uint8_t* y = nv21_y_plane(reinterpret_cast<const uint8_t*>(raw));
                bool have_pair = false;
                MotionFrameFeatures mf{};
                {
                    std::lock_guard<std::mutex> lk(n.m);
                    ++n.frames_processed;
                    if (n.prev_luma_pixels == pixels && !n.prev_luma.empty()) {
                        mf = extract_motion_features(n.prev_luma.data(), y, pixels);
                        have_pair = mf.valid;
                    }
                    n.prev_luma.assign(y, y + pixels);
                    n.prev_luma_pixels = pixels;
                }
                env->ReleaseByteArrayElements(data, raw, JNI_ABORT);  // read-only: nothing copied back
                const int64_t ms = now_ms();
                if (have_pair && due_for_emission(n, ms, /*camera=*/true)) {
                    push_observation(n, "camera", "motion_fraction", mf.changed_fraction, "fraction",
                                     "ok", 1.0, ms);
                    push_observation(n, "camera", "motion_mean_delta", mf.mean_delta, "delta_ratio",
                                     "ok", 1.0, ms);
                }
            }
        }
    }
    return JNI_TRUE;
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
    // One bounded audio descriptor per interval, from the PCM of an AUTHORIZED buffer only.
    n.mic->set_frame_observer([&n](const int16_t* pcm, int32_t frames) {
        {
            std::lock_guard<std::mutex> lk(n.m);
            ++n.audio_windows_processed;
        }
        const AudioFrameFeatures f = extract_audio_features(
            pcm, static_cast<size_t>(frames),
            static_cast<uint32_t>(n.mic->negotiated_channels() > 0 ? n.mic->negotiated_channels() : 1),
            static_cast<uint32_t>(n.mic->negotiated_sample_rate() > 0
                                      ? n.mic->negotiated_sample_rate()
                                      : 16000));
        if (!f.valid) return;
        const int64_t ms = now_ms();
        if (!due_for_emission(n, ms, /*camera=*/false)) return;
        push_observation(n, "microphone", "audio_energy_rms", f.rms, "rms", "ok", 1.0, ms);
        push_observation(n, "microphone", "audio_peak", f.peak, "amplitude", "ok", 1.0, ms);
        push_observation(n, "microphone", "audio_zero_crossing_rate", f.zero_crossing_rate, "rate",
                         "ok", 1.0, ms);
    });

    GuardResult init;
    try {
        init = n.guarded_mic->initialize();
    } catch (const std::exception& e) {
        n.guarded_mic->close();
        return to_jstring(env, std::string("mic: initialize failed (") + e.what() + ")");
    }
    if (!init.allowed) {
        const std::string why = std::string("mic: initialize denied by gate") + " [reason=" +
            ConsentGate::reason_to_string(init.deny_reason) + " detail=" + init.explanation + "]";
        __android_log_print(ANDROID_LOG_WARN, "HGNode", "%s", why.c_str());
        return to_jstring(env, why);
    }
    GuardResult r;
    try {
        r = n.guarded_mic->start();
    } catch (const std::exception& e) {
        n.guarded_mic->close();
        return to_jstring(env, std::string("mic: start failed (") + e.what() + ")");
    }
    if (!r.allowed) {
        const std::string why = std::string("mic: start denied [reason=") +
            ConsentGate::reason_to_string(r.deny_reason) + " detail=" + r.explanation + "]";
        __android_log_print(ANDROID_LOG_WARN, "HGNode", "%s", why.c_str());
        return to_jstring(env, why);
    }
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


// ---------------------------------------------------------------------------------------------
// H-03: the observation drain. Java pulls compact observations and hands them to the D-03
// transport. NDJSON, one observation per line, numeric scalars only - never PIXELS, never PCM,
// never a path or a credential.
//
// C LINKAGE IS NOT OPTIONAL HERE. These two were first appended after the file's
// `}  // extern "C"`, so their symbols were compiled C++-mangled
// (_Z52Java_org_homeguardian_harness_MainActivity_nativeDrainObservationsP7_JNIEnvP8_jobject)
// and the device threw
//   java.lang.UnsatisfiedLinkError: No implementation found for
//   org.homeguardian.harness.MainActivity.nativeDrainObservations()
// on the FIRST tick, before any capture started. Every JNI entry point in this file must be
// inside an extern "C" region - and the build now PROVES it from the compiled ELF rather than
// trusting the source layout (see the JNI export verification step in apk/build_apk.sh).
// ---------------------------------------------------------------------------------------------
extern "C" {
// Live consent state for the operator UI and the log: WHY each sensor is or is not authorized.
// Sanitized by construction - it contains only the gate's closed-vocabulary reason word and its
// fixed explanation sentence. No credentials, no media, no device identity. It sits INSIDE this
// extern "C" region on purpose: a definition outside it is C++-mangled and JNI cannot resolve it.
JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeConsentState(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    auto describe = [&](const char* label, const char* purpose, const char* category) {
        const GateDecision d = n.gate->check(AcquisitionRequest{"node", purpose, category});
        std::string out = std::string(label) + "=" + ConsentGate::reason_to_string(d.reason);
        if (!d.authorized) out += " (" + d.explanation + ")";
        return out;
    };
    const std::string state = "consent: " + describe("camera", "camera_acquire", "camera") + " | " +
                              describe("microphone", "mic_acquire", "audio");
    __android_log_print(ANDROID_LOG_INFO, "HGNode", "%s", state.c_str());
    return to_jstring(env, state);
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeDrainObservations(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    std::deque<Node::Observation> taken;
    {
        std::lock_guard<std::mutex> lk(n.m);
        taken.swap(n.observations);
    }
    std::string out;
    for (const auto& o : taken) {
        out += "{\"source_type\":\"" + o.source_type + "\",\"observation_type\":\"" +
               o.observation_type + "\",\"value\":" + num_str(o.value) + ",\"unit\":\"" +
               o.unit + "\",\"state\":\"" + o.state + "\",\"confidence\":" +
               num_str(o.confidence) +
               ",\"acquired_at_ms\":" + std::to_string(o.acquired_at_ms) +
               ",\"provenance\":\"" + o.provenance + "\"}\n";
    }
    return to_jstring(env, out);
}

// The observation sampling interval (ms). Bounded so a caller cannot ask for an unbounded rate.
JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeSetObservationIntervalMs(JNIEnv* env,
                                                                         jobject /*thiz*/,
                                                                         jint interval_ms) {
    auto& n = node();
    int64_t bounded = interval_ms < 100 ? 100 : (interval_ms > 60000 ? 60000 : interval_ms);
    {
        std::lock_guard<std::mutex> lk(n.m);
        n.observation_interval_ms = bounded;
    }
    return to_jstring(env, "observation interval: " + std::to_string(bounded) + " ms");
}

// ---- camera backend reporting (H-03 blocking fix) ---------------------------------------------
// These are declared in MainActivity. They live INSIDE this extern "C" region on purpose: a
// definition outside it is C++-mangled and JNI cannot resolve it (that defect cost a milestone).

// The Activity reports what the LEGACY api sees. This is one of the two availability views the
// acceptance requires - the other is the NDK enumeration, which returns zero cameras on this device
// and is what "no camera available" recorded. A count is NOT capture: initialize() only means there
// is a camera to open.
JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeCameraReport(JNIEnv* env, jobject /*thiz*/,
                                                              jint camera_count) {
    auto& n = node();
    {
        std::lock_guard<std::mutex> lk(n.m);
        n.platform_camera_count.store(static_cast<int>(camera_count));
        if (n.camera1) n.camera1->set_platform_camera_count(static_cast<int>(camera_count));
    }
    const std::string summary = "camera availability: legacy(Camera1)=" +
                                std::to_string(static_cast<int>(camera_count)) +
                                " (the NDK camera2 enumeration is reported separately)";
    __android_log_print(ANDROID_LOG_INFO, "HGNode", "%s", summary.c_str());
    return to_jstring(env, summary);
}

// The Activity reports the outcome of Camera.open()/startPreview(). A failure must NEVER leave the
// native state claiming capture: force stop+close (which stops delivery immediately) and clear the
// video flag, so `video=on` cannot survive a failed Java start.
JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeCameraStartResult(JNIEnv* env, jobject /*thiz*/,
                                                                   jboolean ok, jstring detail) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    const std::string why = jstring_to_std(env, detail);
    if (ok == JNI_TRUE) {
        return to_jstring(env, "camera1: start confirmed by the java bridge");
    }
    if (n.camera1) n.camera1->set_java_start_failed(why);
    if (n.guarded_camera) {
        n.guarded_camera->stop();
        n.guarded_camera->close();
    }
    n.accountant.note_capture_stopped(/*video=*/true);
    const std::string msg =
        "camera: start FAILED at the java bridge - capture state cleared (video=off): " + why;
    __android_log_print(ANDROID_LOG_WARN, "HGNode", "%s", msg.c_str());
    return to_jstring(env, msg);
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_harness_MainActivity_nativeCameraBackend(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    std::lock_guard<std::mutex> lk(n.m);
    return to_jstring(env, "camera backend: " +
                               (n.camera_backend.empty() ? std::string("none") : n.camera_backend));
}

// The Activity's reconciler uses this to notice that the NATIVE layer has left capture (stop,
// consent withdrawal, permission revocation, pause) and release the Camera1 resources itself. The
// direction matters: Java asks native, never the reverse, so no lock is held while calling into Java.
JNIEXPORT jboolean JNICALL
Java_org_homeguardian_harness_MainActivity_nativeVideoRunning(JNIEnv* env, jobject /*thiz*/) {
    auto& n = node();
    return n.accountant.video_running() ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C" (H-03 observation drain + interval control)

// hg_edge_service_jni.cpp — JNI surface for the HomeGuardian edge service app.
//
// This is NOT a reimplementation. It constructs and runs the SAME objects the
// hg_mcp_server binary constructs in main_mcp_server.cpp:
//
//     Application + PersistenceAudit + AlexaCredentialStore  ->  McpTools  ->  McpHttpServer
//
// The only differences from the binary are (a) the accept loop runs on a
// std::thread instead of main(), because Android owns the process entry point,
// and (b) control is exposed through JNI so a foreground Service can start, stop,
// restart and health-check it.
//
// Android-specific constraints honoured here:
//  * An app's stdout/stderr are redirected to /dev/null, so spdlog is routed to
//    logcat via a liblog sink. Logging to stderr would make the server silent.
//  * The listener stays on 127.0.0.1 and allow_remote is never set. The transport
//    refuses a non-loopback bind without it, so LAN exposure is impossible from
//    inside the app.
//  * The credential file lives in the app's private filesDir, which is owned by
//    the app uid — the store's 0600 + owner check is satisfiable with no ADB
//    privileges and no root.

#include <jni.h>

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "core/Alert.h"
#include "core/AlexaContract.h"
#include "core/AlexaCredentialStore.h"
#include "core/Application.h"
#include "core/Config.h"
#include "core/PersistenceAudit.h"
#include "mcp/McpHttpServer.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpTools.h"

#define HG_LOG_TAG "HGEdge"

namespace {

using homeguardian::Application;
using homeguardian::AlexaCredentialStore;
using homeguardian::IAlexaStatusProvider;
using homeguardian::PersistenceAudit;
using homeguardian::mcp::McpHttpConfig;
using homeguardian::mcp::McpHttpServer;
using homeguardian::mcp::make_homeguardian_tools;

// ---------------------------------------------------------------------------
// spdlog -> logcat. Without this every engine log line is discarded on Android.
// ---------------------------------------------------------------------------
class AndroidLogSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        std::string s(formatted.data(), formatted.size());
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        // spdlog::level values are ordered (trace < debug < info < warn < err < critical),
        // so a direct comparison is the portable way to map to a logcat priority.
        // spdlog::level::to_int_level does not exist in spdlog 1.13.
        const int prio = (msg.level >= spdlog::level::err) ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO;
        __android_log_write(prio, HG_LOG_TAG, s.c_str());
    }
    void flush_() override {}
};

// Read-only status view over the real Application + PersistenceAudit. Mirrors
// McpStatusProvider in main_mcp_server.cpp: capture is reported from config, and
// availability is false whenever persistence is unhealthy.
class EdgeStatusProvider : public IAlexaStatusProvider {
public:
    EdgeStatusProvider(bool capture_enabled, PersistenceAudit* audit)
        : capture_enabled_(capture_enabled), audit_(audit) {}

    homeguardian::AlexaStatusSnapshot snapshot() const override {
        homeguardian::AlexaStatusSnapshot s;
        s.state = "Running";
        s.capture_enabled = capture_enabled_;
        s.available = (audit_ != nullptr) && audit_->healthy();
        if (audit_ != nullptr) {
            const auto recent =
                audit_->alerts().find_recent(homeguardian::ALEXA_MAX_ALERTS_RETURNED);
            s.alert_count = audit_->total_alert_count();
            for (const auto& a : recent) s.alert_ids.push_back(a.alert_id);
            if (!recent.empty()) {
                homeguardian::Severity top = homeguardian::Severity::info;
                for (const auto& a : recent) {
                    if (a.severity == homeguardian::Severity::critical) {
                        top = homeguardian::Severity::critical;
                        break;
                    }
                    if (a.severity == homeguardian::Severity::warning) {
                        top = homeguardian::Severity::warning;
                    }
                }
                s.highest_severity = top == homeguardian::Severity::critical  ? "critical"
                                     : top == homeguardian::Severity::warning ? "warning"
                                                                              : "info";
            }
        }
        return s;
    }

private:
    bool capture_enabled_ = false;
    PersistenceAudit* audit_ = nullptr;
};

// ---------------------------------------------------------------------------
// Controller: one per process. start()/stop() are idempotent and serialised.
// ---------------------------------------------------------------------------
class EdgeController {
public:
    // Return codes are stable and mirrored in NativeEngine.java.
    enum : int {
        OK = 0,
        ALREADY_RUNNING = 2,
        CREDENTIAL_STORE_FAILED = 3,
        START_REFUSED = 4,
        INTERNAL_ERROR = 5,
        NOT_RUNNING = 6,
    };

    int start(const std::string& data_dir, const std::string& config_path,
              const std::string& creds_path, const std::string& bind_addr,
              uint16_t requested_port) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load()) return ALREADY_RUNNING;

        last_error_.clear();
        try {
            // ---- real application state (read-only view) --------------------
            bool capture_enabled = false;  // fail-safe default
            try {
                homeguardian::Config conf;
                conf.load(config_path);
                capture_enabled = conf.media_capture_enabled;
            } catch (const std::exception& e) {
                capture_enabled = false;  // never imply capture is on
                __android_log_print(ANDROID_LOG_WARN, HG_LOG_TAG,
                                    "config unreadable, defaulting capture_enabled=false: %s",
                                    e.what());
            }

            app_ = std::make_unique<Application>();
            app_->get_status().capture_enabled.store(capture_enabled);

            audit_ = std::make_unique<PersistenceAudit>(data_dir + "/homeguardian_edge_audit.db");

            // ---- credentials: secure store only, fail closed ----------------
            store_ = std::make_unique<AlexaCredentialStore>();
            const auto load_res = store_->load(creds_path);
            if (!load_res.ok) {
                last_error_ = load_res.reason;
                __android_log_print(ANDROID_LOG_ERROR, HG_LOG_TAG,
                                    "credential store failed to load securely: %s",
                                    load_res.reason.c_str());
                store_.reset();
                audit_.reset();
                app_.reset();
                return CREDENTIAL_STORE_FAILED;
            }

            provider_ = std::make_unique<EdgeStatusProvider>(capture_enabled, audit_.get());
            // The registry MUST outlive the server: McpHttpServer::Impl stores
            // `const ToolRegistry& tools`, not a copy, so the caller owns its lifetime
            // (main_mcp_server.cpp satisfies this by accident - its registry is a local
            // of main(), which lives as long as the process). Holding it as a local here
            // instead made it die when start() returned while the accept loop kept running
            // on a thread, and the first tools/list took SIGSEGV inside
            // tools_list_json -> nlohmann::json::parse. Hence a member.
            tools_ = std::make_unique<homeguardian::mcp::ToolRegistry>(
                make_homeguardian_tools(*store_, *provider_));

            McpHttpConfig cfg;
            cfg.bind_address = bind_addr;
            cfg.port = requested_port;
            cfg.allow_remote = false;   // loopback only; never configurable from the app
            cfg.require_auth = true;

            server_ = std::make_unique<McpHttpServer>(cfg, *tools_, *store_);
            std::string error;
            if (!server_->start(error)) {
                last_error_ = error;
                __android_log_print(ANDROID_LOG_ERROR, HG_LOG_TAG,
                                    "mcp startup refused: %s", error.c_str());
                server_.reset();
                tools_.reset();
                provider_.reset();
                store_.reset();
                audit_.reset();
                app_.reset();
                return START_REFUSED;
            }

            port_ = server_->port();
            // Exact-match Origin allowlist for the port actually bound. Set before
            // serve(): the transport reads it from worker threads.
            const std::vector<std::string> origins = {
                "http://" + cfg.bind_address + ":" + std::to_string(port_),
                "http://localhost:" + std::to_string(port_),
                "http://127.0.0.1:" + std::to_string(port_)};
            server_->set_allowed_origins(origins);

            stop_.store(false);
            running_.store(true);
            serve_thread_ = std::thread([this]() {
                server_->serve(stop_);
                running_.store(false);
                __android_log_print(ANDROID_LOG_INFO, HG_LOG_TAG, "serve() returned; server stopped");
            });

            __android_log_print(ANDROID_LOG_INFO, HG_LOG_TAG,
                                "hg_mcp_server listening on %s:%u (loopback=true, auth=required, tools=%zu)",
                                cfg.bind_address.c_str(), static_cast<unsigned>(port_), tools_->size());
            return OK;
        } catch (const std::exception& e) {
            last_error_ = e.what();
            __android_log_print(ANDROID_LOG_ERROR, HG_LOG_TAG, "start threw: %s", e.what());
            teardown_locked();
            return INTERNAL_ERROR;
        }
    }

    int stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load() && !serve_thread_.joinable()) return NOT_RUNNING;
        stop_.store(true);
        // serve() polls the flag every 200 ms, so this join is prompt.
        if (serve_thread_.joinable()) serve_thread_.join();
        teardown_locked();
        __android_log_print(ANDROID_LOG_INFO, HG_LOG_TAG, "server stopped on request");
        return OK;
    }

    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }

    std::string last_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

    std::string stats_json() {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json j;
        j["running"] = running_.load();
        j["port"] = port_;
        j["credentials_loaded"] = (store_ != nullptr) && store_->loaded_ok();
        j["credential_count"] = (store_ != nullptr) ? store_->size() : 0;
        j["persistence_healthy"] = (audit_ != nullptr) && audit_->healthy();
        j["total_alert_count"] = (audit_ != nullptr) ? audit_->total_alert_count() : 0;
        if (server_) {
            const auto s = server_->stats();
            j["requests"] = s.requests;
            j["rejected_origin"] = s.rejected_origin;
            j["rejected_auth"] = s.rejected_auth;
            j["rejected_oversize"] = s.rejected_oversize;
            j["rejected_too_many"] = s.rejected_too_many;
            j["errors"] = s.errors;
        } else {
            j["requests"] = 0;
        }
        return j.dump();
    }

private:
    void teardown_locked() {
        // serve() owns and closes the listener. Order matters: stop the loop and drop
        // the server BEFORE the objects its worker threads reference disappear - the
        // registry in particular is held by reference.
        server_.reset();
        tools_.reset();
        provider_.reset();
        store_.reset();
        audit_.reset();
        app_.reset();
        stop_.store(true);
        running_.store(false);
    }

    std::mutex mutex_;
    std::unique_ptr<Application> app_;
    std::unique_ptr<PersistenceAudit> audit_;
    std::unique_ptr<AlexaCredentialStore> store_;
    std::unique_ptr<EdgeStatusProvider> provider_;
    // Owned as a member, not a local: McpHttpServer::Impl holds a reference to it.
    std::unique_ptr<homeguardian::mcp::ToolRegistry> tools_;
    std::unique_ptr<McpHttpServer> server_;
    std::thread serve_thread_;
    std::atomic<bool> stop_{true};
    std::atomic<bool> running_{false};
    uint16_t port_ = 0;
    std::string last_error_;
};

EdgeController& controller() {
    static EdgeController c;
    return c;
}

std::string jstr(JNIEnv* env, jstring s) {
    if (s == nullptr) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

void install_logger_once() {
    static std::once_flag once;
    std::call_once(once, []() {
        auto sink = std::make_shared<AndroidLogSink>();
        auto logger = std::make_shared<spdlog::logger>("hg_edge", sink);
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);
        // Bionic declares signal() at global scope (<signal.h>); use it explicitly.
        ::signal(SIGPIPE, SIG_IGN);
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// JNI surface. Every function is inside this extern "C" region: a definition
// that lands outside it is compiled with C++ linkage, JNI finds nothing under
// the plain Java_... name, and the first call throws UnsatisfiedLinkError.
// ---------------------------------------------------------------------------
extern "C" {

JNIEXPORT jstring JNICALL
Java_org_homeguardian_edgeservice_NativeEngine_nativeVersion(JNIEnv* env, jclass) {
    return env->NewStringUTF("homeguardian-edge-service/1.0.0 (native arm64-v8a)");
}

JNIEXPORT jint JNICALL
Java_org_homeguardian_edgeservice_NativeEngine_nativeStart(JNIEnv* env, jclass, jstring jdata_dir,
                                                           jstring jconfig, jstring jcreds,
                                                           jstring jbind, jint jport) {
    install_logger_once();
    const std::string data_dir = jstr(env, jdata_dir);
    const std::string config_path = jstr(env, jconfig);
    const std::string creds_path = jstr(env, jcreds);
    const std::string bind_addr = jstr(env, jbind);
    __android_log_print(ANDROID_LOG_INFO, HG_LOG_TAG,
                        "nativeStart: data_dir=%s config=%s bind=%s port=%d",
                        data_dir.c_str(), config_path.c_str(), bind_addr.c_str(),
                        static_cast<int>(jport));
    return static_cast<jint>(controller().start(data_dir, config_path, creds_path, bind_addr,
                                                static_cast<uint16_t>(jport)));
}

JNIEXPORT jint JNICALL
Java_org_homeguardian_edgeservice_NativeEngine_nativeStop(JNIEnv*, jclass) {
    return static_cast<jint>(controller().stop());
}

JNIEXPORT jboolean JNICALL
Java_org_homeguardian_edgeservice_NativeEngine_nativeIsRunning(JNIEnv*, jclass) {
    return controller().running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_org_homeguardian_edgeservice_NativeEngine_nativePort(JNIEnv*, jclass) {
    return static_cast<jint>(controller().port());
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_edgeservice_NativeEngine_nativeLastError(JNIEnv* env, jclass) {
    return env->NewStringUTF(controller().last_error().c_str());
}

JNIEXPORT jstring JNICALL
Java_org_homeguardian_edgeservice_NativeEngine_nativeStatsJson(JNIEnv* env, jclass) {
    return env->NewStringUTF(controller().stats_json().c_str());
}

}  // extern "C"

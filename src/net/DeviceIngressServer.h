#pragma once

// The hub-side device ingestion server (milestone D-03, section D).
//
// WHY A SEPARATE BOUNDARY: the existing handwritten HTTP server was designed and
// security-reviewed for LOOOPBACK ONLY, and the brief forbids exposing it or
// rebinding it to 0.0.0.0. This is a different process, on a different port, that
// speaks exactly one protocol: mutually authenticated TLS carrying compact
// observation frames. It serves no dashboard, no commands, no credentials and no
// SQLite - those stay behind the untouched loopback boundary.
//
// CONCURRENCY AND BOUNDS: TLS handshakes happen in a fixed pool of worker
// threads; all shared core state (registry, ingest, detectors, pipeline,
// persistence) is guarded by ONE mutex, so the hub's detection semantics stay
// identical to the single-threaded tests. The accepted-connection queue is
// bounded; a full queue refuses the connection immediately (`queue_full`) rather
// than growing. Every socket has an idle timeout, and the frame cap is enforced
// before allocation.

#include "net/DeviceRegistry.h"
#include "net/IngestDetectors.h"
#include "net/ObservationIngest.h"
#include "net/ReplayState.h"

#include <memory>
#include "net/TlsTransport.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace homeguardian::net {

class DeviceIngressServer {
public:
    struct Config {
        TlsServerConfig tls;
        size_t max_queued_connections = 16;  // bounded accept queue
        size_t max_workers = 2;              // bounded worker threads
        // Durable replay protection: where the per-device sequence watermark is
        // persisted. MANDATORY for the ingress - without it, restarting the hub makes an
        // already-accepted observation acceptable again. The ONLY way to run without it is
        // `allow_ephemeral_replay_protection` below PLUS a loopback bind, which is an
        // isolated-test configuration and is refused for any other bind address.
        std::string replay_state_path;
        // Explicit, loud opt-out for isolated tests only. Still refuses a non-loopback bind.
        bool allow_ephemeral_replay_protection = false;
        // How often the operator's authorization files are re-checked for changes.
        // The check is a stat() per observation and a re-read only when the file
        // changed, so this is a latency bound on "how fast does a revocation take
        // effect", not a polling cost.
        int authorization_refresh_ms = 250;
        bool verbose = false;
    };

    struct Stats {
        std::atomic<uint64_t> connections_accepted{0};
        std::atomic<uint64_t> connections_refused{0};
        std::atomic<uint64_t> handshake_failures{0};
        std::atomic<uint64_t> idle_closes{0};
        std::atomic<uint64_t> messages_accepted{0};
        std::atomic<uint64_t> messages_rejected{0};
        std::atomic<uint64_t> heartbeats{0};
        std::atomic<uint64_t> queue_refusals{0};
        std::atomic<uint64_t> alerts_persisted{0};
        std::atomic<uint64_t> deliveries{0};
    };

    // Two delivery paths, both running under the server's core mutex and both
    // feeding the SAME pipeline: an accepted wire observation (as a
    // SecurityObservation) and a detector-derived observation (already
    // pipeline-shaped). Having two explicit callbacks rather than one keeps the
    // ownership of the conversion in the single place that owns the pipeline.
    struct Sinks {
        ObservationIngest::Sink accepted;                      // SecurityObservation -> pipeline
        std::function<void(const Observation&)> derived;        // derived -> same pipeline
    };

    DeviceIngressServer(Config config, DeviceRegistry& registry, ObservationIngest& ingest,
                        IngestDetectors& detectors, Sinks sinks, std::string audit_db_path,
                        std::string revocation_path = std::string(),
                        std::string registry_path = std::string());
    ~DeviceIngressServer();

    DeviceIngressServer(const DeviceIngressServer&) = delete;
    DeviceIngressServer& operator=(const DeviceIngressServer&) = delete;

    bool start(std::string* error);
    void request_stop();
    // Blocks until request_stop() or a fatal accept error.
    void run();
    uint16_t port() const { return listener_.port(); }
    const Stats& stats() const { return stats_; }
    Config& config() { return config_; }

private:
    void worker_loop();
    void handle_connection(std::unique_ptr<TlsConnection> connection);
    void pump_detectors_locked();
    void availability_loop();
    void sweep_availability_locked();
    // Bound by the registry capacity; retained solely to reauthorize quiet-device
    // transitions against the credential that produced their accepted evidence.
    std::map<std::string, std::string> detector_credentials_;

    // Re-check the operator's authorization files and apply any change. Cheap when
    // nothing changed (two stat() calls) and rate-limited regardless, so a revocation
    // takes effect within `authorization_refresh_ms` even if the filesystem's mtime
    // granularity would hide a same-second edit. Must be called with core_mutex_ held.
    void refresh_authorization_locked();

    // True when authorization state is currently usable. False (with a reason) means
    // the operator's file is gone or unparseable, and the boundary REFUSES rather than
    // continuing under the stale permission it last read.
    bool authorization_available_locked(std::string* detail) const;

    // Load durable replay watermarks and install the two-phase replay hooks.
    bool setup_replay_protection(std::string* error);

public:
    // Replay protection is mandatory for a live ingress. Pure and therefore testable
    // without sockets: returns false with a reason when the configuration would run the
    // boundary without durable replay protection where it matters.
    static bool validate_replay_configuration(const Config& config, std::string* error);

    Config config_;
    DeviceRegistry& registry_;
    ObservationIngest& ingest_;
    IngestDetectors& detectors_;
    Sinks sinks_;
    std::string audit_db_path_;
    std::string revocation_path_;
    std::string registry_path_;
    // Authorization-file change detection. (mtime, size) per file plus a full-read
    // deadline, so detection is reliable without re-parsing on every message.
    int64_t registry_mtime_ = -1;
    size_t registry_size_ = 0;
    int64_t revocation_mtime_ = -1;
    size_t revocation_size_ = 0;
    int64_t last_authorization_read_ms_ = 0;
    std::string authorization_fault_;
    // Durable replay protection. Null only when the operator disabled it, which is
    // reported loudly at startup.
    std::unique_ptr<ReplayState> replay_;
    TlsListener listener_{};
    Stats stats_{};

    std::atomic<bool> stopping_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    // Dedicated to availability_loop(): a maintenance wake-up must never consume
    // the notification a freshly queued connection needs from its worker.
    std::condition_variable maintenance_cv_;
    std::deque<std::unique_ptr<TlsConnection>> queue_{};
    std::vector<std::thread> workers_{};
    std::mutex core_mutex_;  // guards registry_, ingest_, detectors_ and the sink
};

}  // namespace homeguardian::net

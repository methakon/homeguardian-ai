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
#include "net/TlsTransport.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
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
                        std::string revocation_path = std::string());
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

    Config config_;
    DeviceRegistry& registry_;
    ObservationIngest& ingest_;
    IngestDetectors& detectors_;
    Sinks sinks_;
    std::string audit_db_path_;
    std::string revocation_path_;
    TlsListener listener_{};
    Stats stats_{};

    std::atomic<bool> stopping_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::unique_ptr<TlsConnection>> queue_{};
    std::vector<std::thread> workers_{};
    std::mutex core_mutex_;  // guards registry_, ingest_, detectors_ and the sink
};

}  // namespace homeguardian::net

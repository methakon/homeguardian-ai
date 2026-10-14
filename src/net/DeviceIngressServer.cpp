#include "net/DeviceIngressServer.h"

#include <chrono>
#include <iostream>

namespace homeguardian::net {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

DeviceMessage error_message(RejectReason reason, const std::string& detail) {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::error;
    m.reason = reason;
    m.detail = detail;
    return m;
}

DeviceMessage welcome_message(const std::string& device_id) {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::welcome;
    m.device_id_claim = device_id;
    return m;
}

DeviceMessage ack_message(const std::string& observation_id, uint64_t sequence, bool accepted,
                          RejectReason reason, const std::string& detail) {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::acknowledgement;
    m.observation_id = observation_id;
    m.sequence = sequence;
    m.reason = accepted ? RejectReason::none : reason;
    m.detail = detail;
    return m;
}

// Registry AuthResult -> wire reason.
RejectReason reason_for(DeviceRegistry::AuthResult auth) {
    switch (auth) {
        case DeviceRegistry::AuthResult::ok: return RejectReason::none;
        case DeviceRegistry::AuthResult::unknown_device: return RejectReason::device_unknown;
        case DeviceRegistry::AuthResult::revoked: return RejectReason::device_revoked;
        case DeviceRegistry::AuthResult::unpaired: return RejectReason::device_unpaired;
        case DeviceRegistry::AuthResult::credential_mismatch: return RejectReason::identity_mismatch;
        case DeviceRegistry::AuthResult::no_credential: return RejectReason::unauthorized;
    }
    return RejectReason::unauthorized;
}

}  // namespace

DeviceIngressServer::DeviceIngressServer(Config config, DeviceRegistry& registry,
                                         ObservationIngest& ingest, IngestDetectors& detectors,
                                         Sinks sinks, std::string audit_db_path,
                                         std::string revocation_path)
    : config_(std::move(config)),
      registry_(registry),
      ingest_(ingest),
      detectors_(detectors),
      sinks_(std::move(sinks)),
      audit_db_path_(std::move(audit_db_path)),
      revocation_path_(std::move(revocation_path)) {}

DeviceIngressServer::~DeviceIngressServer() {
    request_stop();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    listener_.close_listener();
}

bool DeviceIngressServer::start(std::string* error) {
    if (!listener_.setup(config_.tls, error)) return false;
    if (config_.max_workers == 0) config_.max_workers = 1;
    for (size_t i = 0; i < config_.max_workers; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
    return true;
}

void DeviceIngressServer::request_stop() {
    stopping_.store(true);
    queue_cv_.notify_all();
}

void DeviceIngressServer::run() {
    while (!stopping_.load()) {
        std::string error;
        std::unique_ptr<TlsConnection> connection = listener_.accept_one(&error);
        if (connection == nullptr) {
            if (error == "accept timeout") continue;  // expected: keeps shutdown responsive
            if (error.find("handshake") != std::string::npos) {
                ++stats_.handshake_failures;
            } else {
                ++stats_.connections_refused;
            }
            if (config_.verbose && !error.empty()) {
                std::cerr << "ingress: connection refused: " << error << "\n";
            }
            continue;
        }
        ++stats_.connections_accepted;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.size() >= config_.max_queued_connections) {
                // Bounded: refuse rather than grow. The peer gets a clean error
                // frame and the connection closes, so a flood cannot exhaust the
                // hub's memory or its worker pool.
                ++stats_.queue_refusals;
                connection->write_message(error_message(RejectReason::queue_full,
                                                        "ingress queue is full"));
                continue;
            }
            queue_.push_back(std::move(connection));
        }
        queue_cv_.notify_one();
    }

    // Drain in-flight work before returning.
    request_stop();
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
}

void DeviceIngressServer::worker_loop() {
    for (;;) {
        std::unique_ptr<TlsConnection> connection;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !queue_.empty() || stopping_.load(); });
            if (queue_.empty()) {
                if (stopping_.load()) return;
                continue;
            }
            connection = std::move(queue_.front());
            queue_.pop_front();
        }
        handle_connection(std::move(connection));
    }
}

void DeviceIngressServer::handle_connection(std::unique_ptr<TlsConnection> connection) {
    const PeerIdentity& peer = connection->peer();

    // 1. The TLS layer already verified the chain; require a usable identity.
    if (!peer.authenticated || peer.device_id.empty() || peer.credential_id.empty()) {
        connection->write_message(error_message(RejectReason::unauthorized,
                                                "no verified client identity"));
        return;
    }

    // 2. Bind the certificate to a registry record. Authentication is NOT
    //    permission: this only establishes WHO is calling. The revocation list is
    //    re-read here (bounded, and the normal case is an absent file) so a
    //    revocation takes effect on the device's NEXT connection rather than at the
    //    next restart.
    DeviceRegistry::AuthResult auth = DeviceRegistry::AuthResult::unknown_device;
    {
        std::lock_guard<std::mutex> lock(core_mutex_);
        if (!revocation_path_.empty()) {
            std::string ignore;
            (void)registry_.apply_revocation_file(revocation_path_, &ignore);
        }
        auth = registry_.authenticate(peer.device_id, peer.credential_id);
    }
    if (auth != DeviceRegistry::AuthResult::ok) {
        connection->write_message(
            error_message(reason_for(auth), DeviceRegistry::auth_result_to_string(auth)));
        return;
    }

    // 3. The first frame must be a hello.
    RejectReason reason = RejectReason::none;
    std::string detail;
    DeviceMessage message;
    if (connection->read_message(&message, &reason, &detail) != TlsConnection::ReadOutcome::message ||
        message.kind != MessageKind::hello) {
        connection->write_message(error_message(RejectReason::bad_json, "expected a hello frame"));
        return;
    }
    if (!message.device_id_claim.empty() && message.device_id_claim != peer.device_id) {
        connection->write_message(error_message(RejectReason::identity_mismatch,
                                                "hello claims another device's identity"));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(core_mutex_);
        registry_.note_heartbeat(peer.device_id, now_ms(), message.capabilities);
    }
    if (!connection->write_message(welcome_message(peer.device_id))) return;

    // 4. Serve the observation stream until the peer goes quiet, closes, or sends
    //    something malformed.
    for (;;) {
        if (stopping_.load()) return;
        reason = RejectReason::none;
        detail.clear();
        const TlsConnection::ReadOutcome outcome =
            connection->read_message(&message, &reason, &detail);
        if (outcome == TlsConnection::ReadOutcome::idle_timeout) {
            ++stats_.idle_closes;  // a healthy silence, closed quietly
            return;
        }
        if (outcome == TlsConnection::ReadOutcome::closed) return;
        if (outcome == TlsConnection::ReadOutcome::oversized ||
            outcome == TlsConnection::ReadOutcome::bad_frame ||
            outcome == TlsConnection::ReadOutcome::error) {
            // The stream may be desynchronised: tell the peer why and close. A
            // reconnect is cheap and explicit, and nothing partial is stored.
            connection->write_message(error_message(reason, detail));
            return;
        }

        if (message.kind == MessageKind::goodbye) return;

        if (message.kind == MessageKind::heartbeat) {
            std::lock_guard<std::mutex> lock(core_mutex_);
            registry_.note_heartbeat(peer.device_id, now_ms(), message.capabilities);
            ++stats_.heartbeats;
            continue;
        }

        if (message.kind != MessageKind::observation) {
            connection->write_message(error_message(RejectReason::unknown_kind,
                                                    "only observations are accepted here"));
            return;
        }

        PeerContext context;
        context.authenticated_device_id = peer.device_id;
        context.credential_id = peer.credential_id;
        context.peer_authenticated = true;

        ObservationIngest::Result result;
        {
            std::lock_guard<std::mutex> lock(core_mutex_);
            result = ingest_.accept(context, message, sinks_.accepted);
            pump_detectors_locked();
        }

        if (result.accepted) {
            ++stats_.messages_accepted;
            ++stats_.deliveries;
        } else {
            ++stats_.messages_rejected;
        }
        if (!connection->write_message(ack_message(result.observation_id, result.sequence,
                                                   result.accepted, result.reason, result.detail))) {
            return;
        }
    }
}

void DeviceIngressServer::pump_detectors_locked() {
    // Derived observations from the D-02x detectors go to the SAME pipeline, so
    // there is exactly one event engine and one alert policy.
    std::vector<Observation> derived;
    detectors_.drain(derived);
    if (sinks_.derived) {
        for (const Observation& observation : derived) sinks_.derived(observation);
    }
    // A liveness transition is a real event (a device went quiet), so it follows
    // the accepted-observation path.
    for (const SecurityObservation& availability : detectors_.sweep_availability()) {
        if (sinks_.accepted) sinks_.accepted(availability);
    }
}

}  // namespace homeguardian::net

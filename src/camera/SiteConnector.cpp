#include "SiteConnector.h"

#include <exception>
#include <utility>

namespace homeguardian::camera {

SiteConnector::SiteConnector(ICameraAdapter& adapter,
                             const IDeviceAuthorizer& authorizer,
                             std::string credential_id,
                             EventSink sink,
                             ConnectorConfig config)
    : adapter_(adapter),
      authorizer_(authorizer),
      credential_(std::move(credential_id)),
      sink_(std::move(sink)),
      cfg_(config) {
    status_.adapter_class = adapter_.adapter_class();
    status_.id = adapter_.id();
}

AuthorizationOutcome SiteConnector::authorize(Capability capability) const {
    return authorizer_.authorize(adapter_.id(), credential_, capability);
}

Provenance SiteConnector::provenance_for_adapter() const {
    // A mock is synthetic BY CONSTRUCTION. There is deliberately no override: a test
    // cannot promote a mock to `real`, and a real adapter cannot be downgraded to
    // hide it. This is the rule that keeps "the mock suite passed" from being read as
    // "V380 media works".
    return adapter_.adapter_class() == AdapterClass::mock ? Provenance::synthetic
                                                          : Provenance::real;
}

bool SiteConnector::emit(DetectorStatus detector_status, const std::string& note,
                         uint64_t frames, uint32_t bytes, int64_t now_ms) {
    if (!sink_) { ++status_.observations_suppressed; return false; }

    // Announce an unchanged `unavailable` state only as often as configured. A camera
    // that stays unplugged must not produce a stream of identical failures.
    if (detector_status == DetectorStatus::unavailable) {
        ++consecutive_unavailable_;
        if (has_emitted_ && last_emitted_status_ == DetectorStatus::unavailable) {
            if (cfg_.reannounce_unavailable_every == 0 ||
                consecutive_unavailable_ % cfg_.reannounce_unavailable_every != 0) {
                ++status_.observations_suppressed;
                return false;
            }
        }
    }

    Observation obs;
    obs.observation_id = "cam-" + adapter_.id().canonical() + "-" +
                         std::to_string(++observation_seq_);
    // `source` carries the STABLE logical identity, never an address: both cameras have
    // already changed address once, and identity must not move with them.
    obs.source = "camera:" + adapter_.id().canonical();
    obs.provenance = provenance_for_adapter();
    obs.detector_id = "camera-stream-activity";
    obs.model_id = "boundary-v1";
    obs.schema_version = 1;
    obs.observation_timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(now_ms));
    obs.status = detector_status;
    if (note.size() <= Observation::kMaxNoteChars) obs.note = note;
    obs.features = nlohmann::json::object();
    obs.features["adapter_class"] = adapter_class_to_string(adapter_.adapter_class());
    obs.features["frames_accounted"] = frames;
    obs.features["bytes_accounted"] = bytes;

    try {
        obs.validate();
        sink_(obs.to_event());
    } catch (const std::exception&) {
        // A malformed observation is counted, never silently dropped and never
        // allowed to take down the connector.
        ++status_.observations_rejected;
        return false;
    }

    ++status_.observations_emitted;
    has_emitted_ = true;
    last_emitted_status_ = detector_status;
    consecutive_unavailable_ = 0;
    return true;
}

AuthorizationOutcome SiteConnector::register_device() {
    const AuthorizationOutcome outcome = authorize(Capability::video);
    if (is_denied(outcome)) {
        status_.registered = false;
        status_.last_denial = outcome;
        return outcome;
    }
    status_.registered = true;
    return outcome;
}

ConnectOutcome SiteConnector::start() {
    // 1. Permission. Never inferred from a previous success: a revocation that landed
    //    since the last call must be honoured now.
    const AuthorizationOutcome auth = authorize(Capability::video);
    if (is_denied(auth)) {
        status_.registered = false;
        status_.streaming = false;
        status_.last_denial = auth;
        status_.stopped_by_authorization = true;
        adapter_.stop_stream();
        status_.last_connect = ConnectOutcome::refused_unsafe_interface;
        // No observation is emitted for a device we may not use.
        return status_.last_connect;
    }
    status_.registered = true;

    // 2. Declared capability. A declaration is not a grant, but a missing declaration
    //    is a hard stop: camera 2's shape is exactly "no media interface to offer".
    if (!adapter_.declared_capabilities().has(Capability::video)) {
        status_.streaming = false;
        status_.last_connect = ConnectOutcome::capability_absent;
        emit(DetectorStatus::unavailable,
             "no video capability declared: " + std::string(adapter_class_to_string(adapter_.adapter_class())),
             adapter_.frames_accounted(), 0, 0);
        return status_.last_connect;
    }

    // 3. The adapter's own transport.
    StreamRequest req;
    req.max_frames = cfg_.max_frames_per_pull;
    const ConnectOutcome outcome = adapter_.start_stream(req);
    status_.last_connect = outcome;
    status_.streaming = adapter_.streaming();

    if (outcome == ConnectOutcome::refused_unsafe_interface) {
        // Camera 1's shape: the interface exists and is not safe to use. Report it as
        // a refusal, and leave it stopped.
        emit(DetectorStatus::unavailable,
             "interface refused as unsafe to use", adapter_.frames_accounted(), 0, 0);
    } else if (outcome != ConnectOutcome::ok) {
        emit(DetectorStatus::unavailable,
             std::string("stream not available: ") + connect_outcome_to_string(outcome),
             adapter_.frames_accounted(), 0, 0);
    }
    return outcome;
}

size_t SiteConnector::pull_once(int64_t now_ms) {
    if (!status_.streaming) {
        status_.last_connect = ConnectOutcome::stream_absent;
        emit(DetectorStatus::unavailable, "no active stream", adapter_.frames_accounted(), 0, now_ms);
        return 0;
    }

    // AUTHORIZE BEFORE EACH FRAME. This is the closing point: if consent was withdrawn,
    // the device revoked or capture switched off since the stream started, the stream
    // stops HERE and nothing is accounted for or emitted.
    uint32_t accounted = 0;
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < cfg_.max_frames_per_pull; ++i) {
        const AuthorizationOutcome auth = authorize(Capability::video);
        if (is_denied(auth)) {
            status_.last_denial = auth;
            status_.stopped_by_authorization = true;
            adapter_.stop_stream();
            status_.streaming = false;
            ++status_.pulls_denied;
            // Nothing is emitted: an unauthorized device must never look like a quiet one.
            return accounted;
        }
        FrameMeta meta;
        if (adapter_.read_frames(&meta, 1) != 1) {
            status_.last_connect = adapter_.last_outcome();
            status_.streaming = adapter_.streaming();
            break;
        }
        ++accounted;
        bytes += meta.bytes;
    }

    status_.frames_accounted = adapter_.frames_accounted();
    status_.last_connect = adapter_.last_outcome();
    status_.streaming = adapter_.streaming();
    ++status_.pulls_allowed;

    if (accounted > 0) {
        emit(DetectorStatus::detected, "stream active", accounted, bytes, now_ms);
    } else if (status_.streaming) {
        emit(DetectorStatus::not_detected, "stream running, no frames in this window",
             accounted, 0, now_ms);
    } else {
        emit(DetectorStatus::unavailable,
             std::string("transport ended: ") + connect_outcome_to_string(status_.last_connect),
             adapter_.frames_accounted(), 0, now_ms);
    }
    return accounted;
}

ConnectOutcome SiteConnector::reconnect(int64_t now_ms) {
    if (status_.reconnects >= cfg_.max_reconnect_attempts) {
        ++status_.reconnect_failures;
        status_.last_connect = ConnectOutcome::transport_closed;
        emit(DetectorStatus::unavailable, "reconnect budget exhausted",
             adapter_.frames_accounted(), 0, now_ms);
        return ConnectOutcome::transport_closed;
    }
    ++status_.reconnects;

    adapter_.stop_stream();
    status_.streaming = false;

    // Re-authorize BEFORE restarting. A device revoked while it was offline must not
    // come back just because the network did.
    const AuthorizationOutcome auth = authorize(Capability::video);
    if (is_denied(auth)) {
        status_.registered = false;
        status_.last_denial = auth;
        status_.stopped_by_authorization = true;
        ++status_.reconnect_failures;
        status_.last_connect = ConnectOutcome::refused_unsafe_interface;
        return status_.last_connect;
    }

    const ConnectOutcome probe = adapter_.probe();
    status_.last_connect = probe;
    if (probe != ConnectOutcome::ok) {
        ++status_.reconnect_failures;
        emit(DetectorStatus::unavailable,
             std::string("reconnect probe failed: ") + connect_outcome_to_string(probe),
             adapter_.frames_accounted(), 0, now_ms);
        return probe;
    }
    return start();
}

void SiteConnector::stop() noexcept {
    adapter_.stop_stream();
    status_.streaming = false;
}

}  // namespace homeguardian::camera

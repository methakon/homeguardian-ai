#include "net/ObservationIngest.h"

#include <algorithm>
#include <chrono>

namespace homeguardian::net {

namespace {

std::chrono::system_clock::time_point from_ms(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

bool in_list(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

ObservationIngest::Clock ObservationIngest::system_clock_ms() {
    return [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
}

ObservationIngest::ObservationIngest(DeviceRegistry& registry, Config cfg, Clock clock)
    : registry_(registry), cfg_(std::move(cfg)), clock_(std::move(clock)) {}

void ObservationIngest::reset_counters() {
    accepted_ = 0;
    idempotent_repeats_ = 0;
    rejected_ = 0;
    claim_unsubstantiated_ = 0;
    last_reject_ = RejectReason::none;
}

bool ObservationIngest::is_recent_duplicate(const std::string& device_id,
                                            const std::string& observation_id) {
    const auto it = recent_ids_.find(device_id);
    if (it == recent_ids_.end()) return false;
    return std::find(it->second.begin(), it->second.end(), observation_id) != it->second.end();
}

void ObservationIngest::remember_id(const std::string& device_id, const std::string& observation_id) {
    auto& ids = recent_ids_[device_id];
    ids.push_back(observation_id);
    while (ids.size() > cfg_.max_recent_ids_per_device) ids.pop_front();
}

ObservationIngest::Result ObservationIngest::accept(const PeerContext& peer,
                                                    const DeviceMessage& message,
                                                    const Sink& sink) {
    Result result;
    result.observation_id = message.observation_id;
    result.sequence = message.sequence;
    result.reason = RejectReason::none;

    auto reject = [&](RejectReason reason, const std::string& detail) {
        result.accepted = false;
        result.reason = reason;
        result.detail = detail.size() > 96 ? detail.substr(0, 96) : detail;
        ++rejected_;
        last_reject_ = reason;
        return result;
    };

    // 0. Only an observation may enter here.
    if (message.kind != MessageKind::observation) {
        return reject(RejectReason::bad_json, "message kind is not an observation");
    }

    // 1. The connection itself must be mutually authenticated.
    if (!peer.peer_authenticated) return reject(RejectReason::unauthorized, "peer is not authenticated");

    // 2. The authenticated peer's credential must map to a usable device record.
    const DeviceRegistry::AuthResult auth =
        registry_.authenticate(peer.authenticated_device_id, peer.credential_id);
    if (auth != DeviceRegistry::AuthResult::ok) {
        const RejectReason reason = auth == DeviceRegistry::AuthResult::revoked
                                        ? RejectReason::device_revoked
                                    : auth == DeviceRegistry::AuthResult::unpaired
                                        ? RejectReason::device_unpaired
                                    : auth == DeviceRegistry::AuthResult::credential_mismatch
                                        ? RejectReason::identity_mismatch
                                    : auth == DeviceRegistry::AuthResult::no_credential
                                        ? RejectReason::unauthorized
                                        : RejectReason::device_unknown;
        return reject(reason, std::string("credential binding: ") +
                                  DeviceRegistry::auth_result_to_string(auth));
    }

    // 3. A device may not claim an identity other than the one it authenticated as.
    if (!message.device_id_claim.empty() &&
        message.device_id_claim != peer.authenticated_device_id) {
        return reject(RejectReason::identity_mismatch,
                      "claimed device_id differs from the authenticated identity");
    }

    const DeviceRegistry::Device* device = registry_.find(peer.authenticated_device_id);
    if (device == nullptr) return reject(RejectReason::device_unknown, "device is not registered");

    // 4. Consent and capture are checked SEPARATELY from network trust.
    if (!device->consent_granted) {
        return reject(device->consent_subject.empty() ? RejectReason::consent_missing
                                                      : RejectReason::consent_withdrawn,
                      "device consent is not granted");
    }
    if (!device->capture_enabled) {
        return reject(RejectReason::capture_disabled, "capture is not enabled for this device");
    }

    // 5. Allowlists: an unlisted source or observation type is refused.
    if (!in_list(cfg_.allowed_source_types, message.source_type)) {
        return reject(RejectReason::unknown_source_type, "source_type is not allowlisted");
    }
    if (!in_list(cfg_.allowed_observation_types, message.observation_type)) {
        return reject(RejectReason::unknown_observation_type,
                      "observation_type is not allowlisted");
    }

    // 6. Values: a bounded, non-empty, flat numeric payload.
    if (!message.features.is_object() || message.features.empty()) {
        return reject(RejectReason::invalid_value, "features must be a non-empty object");
    }
    if (message.features.size() > cfg_.max_feature_keys) {
        return reject(RejectReason::invalid_value, "too many feature keys");
    }

    // 7. Timestamps: not in the future, not too old, and not behind this device's
    //    own high-water mark beyond the out-of-order tolerance.
    const int64_t now_ms = clock_();
    if (message.acquired_at_ms > now_ms + cfg_.max_future_skew_ms) {
        return reject(RejectReason::malformed_timestamp, "acquisition timestamp is in the future");
    }
    if (now_ms - message.acquired_at_ms > cfg_.max_age_ms) {
        return reject(RejectReason::malformed_timestamp, "acquisition timestamp is too old");
    }
    if (device->last_observation_at_ms > 0 &&
        message.acquired_at_ms < device->last_observation_at_ms - cfg_.max_out_of_order_ms) {
        return reject(RejectReason::stale_timestamp,
                      "acquisition timestamp is behind the device's own watermark");
    }
    result.out_of_order = device->last_observation_at_ms > 0 &&
                          message.acquired_at_ms < device->last_observation_at_ms;

    // 8. Replay and duplicate suppression.
    //
    //    FIRST the uncertain-outcome case. If the hub durably COMPLETED this very
    //    observation and the acknowledgment never reached the device, the retry must be
    //    ANSWERED, not refused: the watermark legitimately already covers this sequence, so
    //    this check has to precede the sequence test or an unacknowledged observation would
    //    be silently refused forever. Nothing is processed again.
    if (replay_hooks_.is_completed_repeat &&
        replay_hooks_.is_completed_repeat(peer.authenticated_device_id, message.sequence,
                                          message.observation_id)) {
        result.accepted = true;
        result.reason = RejectReason::none;
        result.idempotent_repeat = true;
        result.detail = "already processed; answered from the durable record";
        ++idempotent_repeats_;
        return result;
    }
    if (!registry_.sequence_is_new(peer.authenticated_device_id, message.sequence)) {
        return reject(RejectReason::replayed_sequence, "sequence is not newer than the last accepted");
    }
    if (is_recent_duplicate(peer.authenticated_device_id, message.observation_id)) {
        return reject(RejectReason::duplicate_observation, "observation id was already accepted");
    }

    // 9. Room: a full queue refuses rather than growing.
    if (pending_.size() >= cfg_.max_pending) {
        return reject(RejectReason::queue_full, "ingest queue is full");
    }

    // 10. Provenance is DERIVED here, never taken from the payload - and the
    //     trust facts come from the REGISTRY (the single source of truth) rather
    //     than from whatever the caller put in its PeerContext.
    PeerContext effective = peer;
    effective.device_paired = device->paired;
    effective.device_consent_granted = device->consent_granted;
    effective.capture_enabled = device->capture_enabled;
    const ProvenanceDecision provenance = derive_provenance(effective, message);

    // 11. Build the validated observation that will flow through the pipeline.
    SecurityObservation observation;
    observation.device_id = peer.authenticated_device_id;  // authenticated, not claimed
    observation.source = "node";
    const auto source_type = security_source_type_from_string(message.source_type);
    if (!source_type.has_value()) {
        // An allowlisted string with no SecuritySourceType representation is NOT admissible evidence.
        // Refuse it, for two reasons that both matter:
        //   * `.value()` on the optional throws std::bad_optional_access - an exception raised by a value a
        //     PEER put on the wire, reachable as soon as an operator extends the allowlist with a name the
        //     record type cannot represent (HG-CPP-ANDROID-VECTOR-CONSUMER-27: the Android producer sends
        //     source_type "camera", which is exactly this case).
        //   * defaulting it instead would silently relabel what a device reported, which is worse than a
        //     refusal: the vocabulary is the receiver's, and an unknown name must not become "motion".
        return reject(RejectReason::unknown_source_type, "source_type has no known representation");
    }
    observation.source_type = *source_type;
    observation.acquired_at = from_ms(message.acquired_at_ms);
    observation.ingested_at = from_ms(now_ms);
    observation.provenance = provenance.real ? Provenance::real : Provenance::synthetic;
    observation.confidence = message.confidence;
    observation.quality = message.quality;
    observation.state = message.features.value("state", std::string("observed"));
    // Consent context comes from the REGISTRY, never from the wire.
    observation.consent_subject = device->consent_subject;
    observation.consent_purpose = device->consent_purpose;
    observation.authorized = true;  // every check above passed
    observation.features = message.features;
    observation.features["observation_type"] = message.observation_type;
    observation.features["acquisition_mode"] = message.acquisition_mode;
    observation.features["sequence"] = static_cast<uint64_t>(message.sequence);
    observation.features["provenance_claim"] = message.provenance_claim;
    if (message.scale.has_value()) observation.features["scale"] = message.scale.value();
    if (!message.unit.empty()) observation.features["unit"] = message.unit;

    std::string note = "accepted from authenticated device; " + provenance.reason;
    if (result.out_of_order) note += "; out-of-order within tolerance";
    observation.note = note.size() > SecurityObservation::kMaxNoteChars
                           ? note.substr(0, SecurityObservation::kMaxNoteChars)
                           : note;

    try {
        observation.validate();
    } catch (const std::exception& e) {
        return reject(RejectReason::invalid_value, std::string("observation invalid: ") + e.what());
    }

    // 12. TWO-PHASE DURABLE RECORD: BEGIN -> process -> COMPLETE.
    //
    //     WHAT "accepted" MEANS (documented contract): the observation was authenticated,
    //     authorized, bounded and validated, the sink accepted it without throwing, and the
    //     hub recorded its completion durably. It does NOT claim that the sink's own store
    //     holds an alert forever (the sink owns that), and it does NOT claim exactly-once
    //     processing - see docs/DISTRIBUTED_DEVICE_TRANSPORT_D03.md for the full contract and
    //     for the crash window that remains.
    //
    //     ORDERING RULE: the watermark may only ever move AFTER processing succeeded, never
    //     before. Committing first (as this code did) let a downstream failure look like
    //     success in the durable state, so the device's retry was refused as a replay and the
    //     observation was lost with nobody told.
    bool durable_in_flight = false;
    if (replay_hooks_.begin) {
        std::string why;
        const DurableBegin began = replay_hooks_.begin(peer.authenticated_device_id,
                                                       message.sequence, message.observation_id, &why);
        switch (began) {
            case DurableBegin::marked:
                durable_in_flight = true;
                break;
            case DurableBegin::already_completed:
                // A concurrent attempt completed it between the pre-check and here.
                result.accepted = true;
                result.reason = RejectReason::none;
                result.idempotent_repeat = true;
                result.detail = "already processed; answered from the durable record";
                ++idempotent_repeats_;
                return result;
            case DurableBegin::concurrent:
                return reject(RejectReason::duplicate_observation,
                              why.empty() ? "the same sequence is already in flight" : why);
            case DurableBegin::replayed:
                return reject(RejectReason::replayed_sequence,
                              why.empty() ? "sequence is at or behind the completed watermark" : why);
            case DurableBegin::unavailable:
            default:
                return reject(RejectReason::processing_failed,
                              why.empty() ? "durable replay state unavailable" : why);
        }
    }

    pending_.push_back(std::move(observation));
    if (sink) {
        try {
            drain(sink);
        } catch (const std::exception& e) {
            // DROP the failed item: it was refused, so leaving it queued would deliver it a
            // second time when the device retries. Then ABANDON the durable marker, because
            // nothing consumed this sequence - the retry must be accepted, not called a
            // replay.
            if (!pending_.empty()) pending_.pop_front();
            if (durable_in_flight && replay_hooks_.abandon) {
                replay_hooks_.abandon(peer.authenticated_device_id, message.sequence);
            }
            return reject(RejectReason::processing_failed,
                          std::string("downstream processing failed: ") + e.what());
        } catch (...) {
            if (!pending_.empty()) pending_.pop_front();
            if (durable_in_flight && replay_hooks_.abandon) {
                replay_hooks_.abandon(peer.authenticated_device_id, message.sequence);
            }
            return reject(RejectReason::processing_failed, "downstream processing failed");
        }
    }

    // 13. COMPLETE only after processing succeeded. If the completion cannot be recorded,
    //     the acceptance is NOT reported: an acceptance the hub cannot stand behind is
    //     exactly the lie this contract forbids. The in-flight marker is deliberately left in
    //     place so the retry is accepted (bounded at-least-once) rather than refused as a
    //     replay (silent loss).
    if (durable_in_flight) {
        std::string why;
        if (!replay_hooks_.complete(peer.authenticated_device_id, message.sequence,
                                    message.observation_id, &why)) {
            return reject(RejectReason::processing_failed,
                          why.empty() ? "durable replay state could not be advanced" : why);
        }
    }

    // 14. Commit the stream state, so neither a refused message nor a failed delivery
    //     consumes a sequence.
    registry_.commit_sequence(peer.authenticated_device_id, message.sequence);
    registry_.note_observation_timestamp(peer.authenticated_device_id, message.acquired_at_ms);
    remember_id(peer.authenticated_device_id, message.observation_id);

    result.accepted = true;
    result.reason = RejectReason::none;
    result.real = provenance.real;
    result.provenance_claim_unsubstantiated = provenance.claim_unsubstantiated;
    ++accepted_;
    if (provenance.claim_unsubstantiated) ++claim_unsubstantiated_;
    return result;
}

size_t ObservationIngest::drain(const Sink& sink) {
    if (!sink) return 0;
    size_t delivered = 0;
    while (!pending_.empty()) {
        sink(pending_.front());
        pending_.pop_front();
        ++delivered;
    }
    return delivered;
}

}  // namespace homeguardian::net

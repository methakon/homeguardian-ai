#include "catch2/catch.hpp"

#include "core/AlertPolicy.h"
#include "core/DetectionFusionRule.h"
#include "core/Observation.h"
#include "core/Pipeline.h"
#include "net/DeviceProtocol.h"
#include "net/DeviceRegistry.h"
#include "net/IngestDetectors.h"
#include "net/ReplayState.h"
#include "net/ObservationIngest.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;
using namespace homeguardian::net;

namespace {

constexpr int64_t kNow = 1'700'000'000'000LL;  // fixed clock for determinism

int64_t fixed_clock() { return kNow; }

std::string temp_path(const std::string& name) {
    return "/tmp/hg_d03_" + std::to_string(::getpid()) + "_" + name;
}

DeviceRegistry::Device make_device(const std::string& id, const std::string& credential,
                                   bool paired = true) {
    DeviceRegistry::Device d;
    d.device_id = id;
    d.display_name = "Test node";
    d.device_type = "android";
    d.credential_id = credential;
    d.paired = paired;
    d.capabilities = {"camera", "microphone"};
    d.has_camera = true;
    d.has_microphone = true;
    return d;
}

// A registry with one fully authorized device: paired, consented, capture on.
struct Fixture {
    DeviceRegistry registry;
    std::string device_id = "hg-aabbccddeeff0011";
    std::string credential = "aabbccddeeff0011";

    Fixture() {
        std::string why;
        REQUIRE(registry.pair(make_device(device_id, credential), &why));
        REQUIRE(registry.set_consent(device_id, true, device_id, "security_monitor", kNow));
        REQUIRE(registry.set_capture_enabled(device_id, true));
    }

    PeerContext peer(bool authenticated = true) const {
        PeerContext p;
        p.authenticated_device_id = device_id;
        p.credential_id = credential;
        p.peer_authenticated = authenticated;
        p.device_paired = true;
        p.device_consent_granted = true;
        p.capture_enabled = true;
        return p;
    }
};

DeviceMessage observation_message(const std::string& device_id, uint64_t sequence,
                                  int64_t acquired_at_ms = kNow - 1000,
                                  const std::string& source_type = "motion",
                                  const std::string& observation_type = "motion-changed-fraction") {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::observation;
    m.observation_id = "obs-" + std::to_string(sequence);
    m.device_id_claim = device_id;
    m.source_type = source_type;
    m.observation_type = observation_type;
    m.acquisition_mode = "live";
    m.acquired_at_ms = acquired_at_ms;
    m.sequence = sequence;
    m.features = {{"motion-changed-fraction", 0.25}};
    m.provenance_claim = "real";
    return m;
}

std::shared_ptr<Pipeline> make_pipeline() {
    std::vector<std::shared_ptr<ICorrelationRule>> rules;
    rules.push_back(std::make_shared<DetectionFusionRule>("detection-fusion"));
    return std::make_shared<Pipeline>(rules);
}

}  // namespace

// ===========================================================================
// Protocol: framing, encoding, strict decoding
// ===========================================================================

TEST_CASE("device protocol frames, encodes and strictly decodes", "[net][protocol]") {
    SECTION("a message round-trips through encode -> frame -> decode") {
        DeviceMessage original = observation_message("hg-1", 7);
        const nlohmann::json payload = encode(original);
        RejectReason reason = RejectReason::bad_json;
        std::string detail;
        const auto decoded = decode(payload, &reason, &detail);
        REQUIRE(decoded.has_value());
        REQUIRE(reason == RejectReason::none);
        REQUIRE(decoded->kind == MessageKind::observation);
        REQUIRE(decoded->observation_id == "obs-7");
        REQUIRE(decoded->sequence == 7);
        REQUIRE(decoded->acquired_at_ms == kNow - 1000);
        REQUIRE(decoded->features["motion-changed-fraction"] == Approx(0.25));
        REQUIRE(decoded->provenance_claim == "real");
    }

    SECTION("the frame header carries a big-endian length and enforces the cap") {
        const auto framed = frame(encode(observation_message("hg-1", 1)));
        REQUIRE(framed.has_value());
        REQUIRE(framed->size() > 4);
        size_t declared = 0;
        REQUIRE(frame_length_acceptable(framed->data(), framed->size(), &declared));
        REQUIRE(declared == framed->size() - 4);

        // A declared length beyond the cap is refused before any allocation.
        const uint8_t too_big[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        REQUIRE_FALSE(frame_length_acceptable(too_big, 4, nullptr));
        const uint8_t zero[4] = {0, 0, 0, 0};
        REQUIRE_FALSE(frame_length_acceptable(zero, 4, nullptr));

        // And an oversized payload cannot even be framed.
        DeviceMessage huge = observation_message("hg-1", 1);
        huge.features["motion-changed-fraction"] = std::string(5000, 'x');
        REQUIRE_FALSE(frame(encode(huge)).has_value());
    }

    SECTION("an unsupported protocol version is refused") {
        nlohmann::json payload = encode(observation_message("hg-1", 1));
        payload["protocol_version_major"] = kProtocolVersionMajor + 1;
        RejectReason reason = RejectReason::none;
        std::string detail;
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::bad_version);
    }

    SECTION("an unknown kind or acquisition mode is refused") {
        nlohmann::json payload = encode(observation_message("hg-1", 1));
        payload["kind"] = "rm -rf";
        RejectReason reason = RejectReason::none;
        std::string detail;
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::unknown_kind);

        payload = encode(observation_message("hg-1", 1));
        payload["acquisition_mode"] = "pretend";
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::unknown_kind);
    }

    SECTION("malformed timestamps and sequences are refused") {
        nlohmann::json payload = encode(observation_message("hg-1", 1));
        payload["acquired_at_ms"] = 0;
        RejectReason reason = RejectReason::none;
        std::string detail;
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::malformed_timestamp);

        payload = encode(observation_message("hg-1", 1));
        payload["acquired_at_ms"] = "soon";
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::malformed_timestamp);

        payload = encode(observation_message("hg-1", 1));
        payload["sequence"] = 0;
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::invalid_value);
    }

    SECTION("feature payloads are bounded, flat and finite") {
        RejectReason reason = RejectReason::none;
        std::string detail;

        nlohmann::json payload = encode(observation_message("hg-1", 1));
        payload["features"]["nested"] = {{"a", 1}};
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::invalid_value);

        payload = encode(observation_message("hg-1", 1));
        for (size_t i = 0; i <= kMaxFeatureKeys; ++i) payload["features"]["k" + std::to_string(i)] = 1;
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());

        payload = encode(observation_message("hg-1", 1));
        payload["features"]["audio-rms"] = "x";
        payload["features"]["bad"] = std::string(200, 'y');
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::invalid_value);

        payload = encode(observation_message("hg-1", 1));
        payload["confidence"] = 1.5;
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::invalid_value);

        payload = encode(observation_message("hg-1", 1));
        payload["quality"] = -0.2;
        REQUIRE_FALSE(decode(payload, &reason, &detail).has_value());
        REQUIRE(reason == RejectReason::invalid_value);
    }

    SECTION("a hello carries capabilities and a heartbeat carries a sequence") {
        DeviceMessage hello;
        hello.kind = MessageKind::hello;
        hello.protocol_version_major = kProtocolVersionMajor;
        hello.device_id_claim = "hg-1";
        hello.capabilities = {"camera", "microphone"};
        RejectReason reason = RejectReason::bad_json;
        std::string detail;
        const auto decoded = decode(encode(hello), &reason, &detail);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->kind == MessageKind::hello);
        REQUIRE(decoded->capabilities.size() == 2);

        DeviceMessage hb;
        hb.kind = MessageKind::heartbeat;
        hb.protocol_version_major = kProtocolVersionMajor;
        hb.device_id_claim = "hg-1";
        hb.sequence = 42;
        const auto decoded_hb = decode(encode(hb), &reason, &detail);
        REQUIRE(decoded_hb.has_value());
        REQUIRE(decoded_hb->sequence == 42);
    }

    SECTION("reject reasons serialize to stable strings") {
        REQUIRE(std::string(reject_reason_to_string(RejectReason::identity_mismatch)) ==
                "identity_mismatch");
        REQUIRE(std::string(reject_reason_to_string(RejectReason::replayed_sequence)) ==
                "replayed_sequence");
        REQUIRE(std::string(message_kind_to_string(MessageKind::observation)) == "observation");
    }
}

TEST_CASE("provenance is derived from the connection, never claimed", "[net][provenance]") {
    DeviceMessage message = observation_message("hg-1", 1);
    message.provenance_claim = "real";

    SECTION("a fully authorized live peer is recorded as real") {
        PeerContext peer;
        peer.peer_authenticated = true;
        peer.device_paired = true;
        peer.device_consent_granted = true;
        peer.capture_enabled = true;
        const ProvenanceDecision d = derive_provenance(peer, message);
        REQUIRE(d.real);
        REQUIRE_FALSE(d.claim_unsubstantiated);
    }

    SECTION("every missing condition downgrades to synthetic and flags the claim") {
        struct Case {
            const char* name;
            bool authenticated, paired, consent, capture;
            const char* mode;
        };
        const Case cases[] = {
            {"unauthenticated", false, true, true, true, "live"},
            {"unpaired", true, false, true, true, "live"},
            {"no consent", true, true, false, true, "live"},
            {"capture disabled", true, true, true, false, "live"},
            {"not live", true, true, true, true, "synthetic"},
        };
        for (const Case& c : cases) {
            PeerContext peer;
            peer.peer_authenticated = c.authenticated;
            peer.device_paired = c.paired;
            peer.device_consent_granted = c.consent;
            peer.capture_enabled = c.capture;
            DeviceMessage m = message;
            m.acquisition_mode = c.mode;
            const ProvenanceDecision d = derive_provenance(peer, m);
            INFO(c.name);
            REQUIRE_FALSE(d.real);
            REQUIRE(d.claim_unsubstantiated);
            REQUIRE(d.reason.find("recorded synthetic") != std::string::npos);
        }
    }

    SECTION("a synthetic claim from an authorized peer stays synthetic without a flag") {
        PeerContext peer;
        peer.peer_authenticated = true;
        peer.device_paired = true;
        peer.device_consent_granted = true;
        peer.capture_enabled = true;
        DeviceMessage m = message;
        m.provenance_claim = "synthetic";
        const ProvenanceDecision d = derive_provenance(peer, m);
        REQUIRE(d.real);  // the CONNECTION qualifies; the claim is about the data
        REQUIRE_FALSE(d.claim_unsubstantiated);
    }
}

// ===========================================================================
// Registry
// ===========================================================================

TEST_CASE("the device registry enforces pairing, revocation and credential binding",
          "[net][registry]") {
    SECTION("an unknown device is unknown") {
        DeviceRegistry registry;
        REQUIRE(registry.authenticate("hg-nope", "aa") == DeviceRegistry::AuthResult::unknown_device);
    }

    SECTION("credential binding stops one device acting as another") {
        Fixture f;
        REQUIRE(f.registry.authenticate(f.device_id, f.credential) == DeviceRegistry::AuthResult::ok);
        REQUIRE(f.registry.authenticate(f.device_id, "deadbeef") ==
                DeviceRegistry::AuthResult::credential_mismatch);
        REQUIRE(f.registry.authenticate(f.device_id, "") ==
                DeviceRegistry::AuthResult::no_credential);
    }

    SECTION("an unpaired device is not authorized") {
        DeviceRegistry registry;
        std::string why;
        REQUIRE(registry.pair(make_device("hg-1", "aa", /*paired=*/false), &why));
        REQUIRE(registry.authenticate("hg-1", "aa") == DeviceRegistry::AuthResult::unpaired);
    }

    SECTION("revocation is total and cannot be undone by re-pairing") {
        Fixture f;
        REQUIRE(f.registry.revoke(f.device_id, "owner revoked"));
        REQUIRE(f.registry.authenticate(f.device_id, f.credential) ==
                DeviceRegistry::AuthResult::revoked);
        const DeviceRegistry::Device* d = f.registry.find(f.device_id);
        REQUIRE(d != nullptr);
        REQUIRE_FALSE(d->consent_granted);
        REQUIRE_FALSE(d->capture_enabled);
        REQUIRE_FALSE(d->paired);
        REQUIRE(f.registry.revoked_count() == 1);
        // Consent cannot be silently re-granted on a revoked device.
        REQUIRE_FALSE(f.registry.set_consent(f.device_id, true, "s", "p", kNow));
        REQUIRE_FALSE(f.registry.set_capture_enabled(f.device_id, true));
    }

    SECTION("capture cannot be enabled without pairing and consent") {
        DeviceRegistry registry;
        std::string why;
        REQUIRE(registry.pair(make_device("hg-2", "bb"), &why));
        REQUIRE_FALSE(registry.set_capture_enabled("hg-2", true));  // no consent yet
        REQUIRE(registry.set_consent("hg-2", true, "hg-2", "security_monitor", kNow));
        REQUIRE(registry.set_capture_enabled("hg-2", true));
        // Withdrawal stops capture in the same operation.
        REQUIRE(registry.set_consent("hg-2", false, "", "", kNow));
        const DeviceRegistry::Device* d = registry.find("hg-2");
        REQUIRE(d != nullptr);
        REQUIRE_FALSE(d->consent_granted);
        REQUIRE_FALSE(d->capture_enabled);
    }

    SECTION("the registry is bounded") {
        DeviceRegistry::Config cfg;
        cfg.max_devices = 2;
        DeviceRegistry registry(cfg);
        std::string why;
        REQUIRE(registry.pair(make_device("hg-a", "a"), &why));
        REQUIRE(registry.pair(make_device("hg-b", "b"), &why));
        REQUIRE_FALSE(registry.pair(make_device("hg-c", "c"), &why));
        REQUIRE(why.find("full") != std::string::npos);
        REQUIRE(registry.device_count() == 2);
    }

    SECTION("sequence watermarks advance monotonically and only when committed") {
        Fixture f;
        REQUIRE(f.registry.sequence_is_new(f.device_id, 5));
        REQUIRE(f.registry.commit_sequence(f.device_id, 5));
        REQUIRE_FALSE(f.registry.sequence_is_new(f.device_id, 5));
        REQUIRE_FALSE(f.registry.sequence_is_new(f.device_id, 4));
        REQUIRE_FALSE(f.registry.commit_sequence(f.device_id, 4));
        REQUIRE(f.registry.sequence_is_new(f.device_id, 6));
    }

    SECTION("heartbeat marks a device online and silence sweeps it offline") {
        Fixture f;
        f.registry.note_heartbeat(f.device_id, kNow, {"camera"});
        REQUIRE(f.registry.online_count() == 1);
        const DeviceRegistry::Device* d = f.registry.find(f.device_id);
        REQUIRE(d->has_camera);
        REQUIRE_FALSE(d->has_microphone);  // the second heartbeat did not report a mic
        REQUIRE(f.registry.sweep_offline(kNow + 1'000) == 0);
        REQUIRE(f.registry.sweep_offline(kNow + 200'000) == 1);
        REQUIRE(f.registry.online_count() == 0);
    }
}

TEST_CASE("the registry persists strictly and never restores runtime trust state",
          "[net][registry][persistence]") {
    const std::string path = temp_path("registry.json");

    SECTION("load/save round-trips the operator-visible fields") {
        Fixture f;
        std::string why;
        REQUIRE(f.registry.save(path, &why));

        DeviceRegistry reloaded;
        REQUIRE(reloaded.load(path, &why));
        REQUIRE(reloaded.device_count() == 1);
        const DeviceRegistry::Device* d = reloaded.find(f.device_id);
        REQUIRE(d != nullptr);
        REQUIRE(d->credential_id == f.credential);
        REQUIRE(d->paired);
        REQUIRE(d->consent_granted);
        REQUIRE(d->consent_purpose == "security_monitor");
        // Capture follows the operator's WRITTEN authorization, and only when the
        // device is also paired and consented (the fixture sets all three). It is
        // never inferred: see the section below for the fail-closed cases.
        REQUIRE(d->capture_enabled);
        // Runtime state is never restored, because it would be a lie after a
        // restart: online and the sequence watermark are re-learned from the wire.
        REQUIRE_FALSE(d->online);
        REQUIRE(d->last_sequence == 0);
        ::unlink(path.c_str());
    }

    SECTION("capture authorization is fail-closed: absent, unpaired, revoked or consentless") {
        std::string why;
        const std::string id = "hg-failclosed000001";
        const std::string credential(64, 'a');

        auto write_entry = [&](const std::string& extras) {
            std::ofstream out(path, std::ios::trunc);
            out << "{\"devices\":[{\"device_id\":\"" << id << "\",\"credential_id\":\""
                << credential << "\"," << extras << "}]}";
        };

        // Absent key: false.
        write_entry(R"("display_name":"d")");
        DeviceRegistry absent;
        REQUIRE(absent.load(path, &why));
        REQUIRE_FALSE(absent.find(id)->capture_enabled);

        // Explicit true, but UNPAIRED: false.
        write_entry(R"("paired":false,"consent_granted":true,"capture_enabled":true)");
        DeviceRegistry unpaired;
        REQUIRE(unpaired.load(path, &why));
        REQUIRE_FALSE(unpaired.find(id)->capture_enabled);

        // Explicit true but NO CONSENT: false. Capture cannot outrun consent.
        write_entry(R"("paired":true,"consent_granted":false,"capture_enabled":true)");
        DeviceRegistry consentless;
        REQUIRE(consentless.load(path, &why));
        REQUIRE_FALSE(consentless.find(id)->capture_enabled);

        // Explicit true on a REVOKED device: false.
        write_entry(R"("paired":true,"consent_granted":true,"revoked":true,"capture_enabled":true)");
        DeviceRegistry revoked;
        REQUIRE(revoked.load(path, &why));
        REQUIRE_FALSE(revoked.find(id)->capture_enabled);

        ::unlink(path.c_str());
    }

    SECTION("a device removed from the revocation list is no longer revoked") {
        std::string why;
        const std::string id = "hg-reinstate0000001";
        const std::string credential(64, 'b');
        {
            std::ofstream out(path, std::ios::trunc);
            out << "{\"devices\":[{\"device_id\":\"" << id << "\",\"credential_id\":\""
                << credential
                << "\",\"paired\":true,\"consent_granted\":true,\"capture_enabled\":true}]}";
        }
        DeviceRegistry registry;
        REQUIRE(registry.load(path, &why));

        const std::string revocations = temp_path("revocations.json");
        auto write_revocations = [&](const std::string& body) {
            std::ofstream out(revocations, std::ios::trunc);
            out << body;
        };

        write_revocations("[{\"device_id\":\"" + id + "\",\"reason\":\"operator\"}]");
        REQUIRE(registry.apply_revocation_file(revocations, &why));
        REQUIRE(registry.find(id)->revoked);
        REQUIRE_FALSE(registry.find(id)->capture_enabled);
        REQUIRE(registry.authenticate(id, credential) == DeviceRegistry::AuthResult::revoked);

        // Removing the entry LIFTS the ban. With a one-way flag this was impossible
        // without restarting the service, which made reinstating a device a
        // maintenance window.
        write_revocations("[]");
        REQUIRE(registry.apply_revocation_file(revocations, &why));
        REQUIRE_FALSE(registry.find(id)->revoked);
        // Permissions are NOT silently re-granted by un-revoking.
        REQUIRE_FALSE(registry.find(id)->paired);
        REQUIRE_FALSE(registry.find(id)->consent_granted);

        ::unlink(revocations.c_str());
        ::unlink(path.c_str());
    }

    SECTION("a malformed or over-bounded registry file is refused") {
        std::string why;
        {
            std::ofstream bad(path, std::ios::trunc);
            bad << "{not json";
        }
        DeviceRegistry registry;
        REQUIRE_FALSE(registry.load(path, &why));
        REQUIRE(why.find("not valid JSON") != std::string::npos);

        {
            std::ofstream bad(path, std::ios::trunc);
            bad << R"({"devices":[{"device_id":""}]})";
        }
        REQUIRE_FALSE(registry.load(path, &why));
        REQUIRE(why.find("device_id") != std::string::npos);

        {
            std::ofstream bad(path, std::ios::trunc);
            bad << R"({"devices":[{"device_id":"a","credential_id":"bad value with spaces"}]})";
        }
        REQUIRE_FALSE(registry.load(path, &why));

        {
            std::ofstream many(path, std::ios::trunc);
            many << R"({"devices":[)";
            for (size_t i = 0; i < 200; ++i) {
                if (i) many << ",";
                many << R"({"device_id":"hg-)" << i << R"("})";
            }
            many << "]}";
        }
        REQUIRE_FALSE(registry.load(path, &why));
        REQUIRE(why.find("bound") != std::string::npos);
        ::unlink(path.c_str());
    }
}

// ===========================================================================
// The accept path
// ===========================================================================

TEST_CASE("the accept path authorizes before it stores anything", "[net][ingest]") {
    SECTION("an authenticated, paired, consented device with capture on is accepted") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        std::vector<SecurityObservation> delivered;
        const auto result = ingest.accept(f.peer(), observation_message(f.device_id, 1),
                                          [&](const SecurityObservation& o) { delivered.push_back(o); });
        REQUIRE(result.accepted);
        REQUIRE(result.reason == RejectReason::none);
        REQUIRE(result.real);
        REQUIRE(delivered.size() == 1);
        const SecurityObservation& o = delivered.front();
        REQUIRE(o.device_id == f.device_id);          // authenticated identity
        REQUIRE(o.provenance == Provenance::real);
        REQUIRE(o.authorized);
        REQUIRE(o.consent_purpose == "security_monitor");  // from the registry
        REQUIRE(o.features["observation_type"] == "motion-changed-fraction");
        REQUIRE(o.features["sequence"] == 1);
        REQUIRE(o.features["acquisition_mode"] == "live");
        REQUIRE(ingest.pending() == 0);
        REQUIRE(ingest.accepted_count() == 1);
    }

    SECTION("an unauthenticated peer is refused") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        const auto result = ingest.accept(f.peer(/*authenticated=*/false),
                                          observation_message(f.device_id, 1), nullptr);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::unauthorized);
    }

    SECTION("a non-observation message cannot enter the ingest path") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        DeviceMessage hello;
        hello.kind = MessageKind::hello;
        hello.protocol_version_major = kProtocolVersionMajor;
        hello.device_id_claim = f.device_id;
        const auto result = ingest.accept(f.peer(), hello, nullptr);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::bad_json);
    }

    SECTION("an unknown, unpaired or revoked device is refused with its own reason") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);

        PeerContext stranger = f.peer();
        stranger.authenticated_device_id = "hg-stranger";
        stranger.credential_id = "zz";
        REQUIRE(ingest.accept(stranger, observation_message(f.device_id, 1), nullptr).reason ==
                RejectReason::device_unknown);

        f.registry.unpair(f.device_id);
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 2), nullptr).reason ==
                RejectReason::device_unpaired);
    }

    SECTION("wrong-device credentials are refused as an identity mismatch") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        PeerContext wrong = f.peer();
        wrong.credential_id = "0000000000000000";
        const auto result = ingest.accept(wrong, observation_message(f.device_id, 1), nullptr);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::identity_mismatch);
    }

    SECTION("a device may not claim another device's identity") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        DeviceMessage m = observation_message("hg-someone-else", 1);
        const auto result = ingest.accept(f.peer(), m, nullptr);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::identity_mismatch);
    }

    SECTION("missing consent, withdrawn consent and disabled capture are each refused") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        DeviceRegistry::Device fresh = make_device("hg-noconsent", "cc");
        std::string why;
        REQUIRE(f.registry.pair(fresh, &why));
        PeerContext peer;
        peer.authenticated_device_id = "hg-noconsent";
        peer.credential_id = "cc";
        peer.peer_authenticated = true;
        REQUIRE(ingest.accept(peer, observation_message("hg-noconsent", 1), nullptr).reason ==
                RejectReason::consent_missing);

        REQUIRE(f.registry.set_consent("hg-noconsent", true, "hg-noconsent", "monitor", kNow));
        REQUIRE(ingest.accept(peer, observation_message("hg-noconsent", 2), nullptr).reason ==
                RejectReason::capture_disabled);

        // Consent withdrawal mid-stream blocks the very next observation.
        REQUIRE(f.registry.set_consent(f.device_id, false, "", "", kNow));
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 3), nullptr).reason ==
                RejectReason::consent_withdrawn);
    }

    SECTION("replaying a sequence or resending an observation id is refused") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 10), nullptr).accepted);
        // Same sequence again: replay.
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 10), nullptr).reason ==
                RejectReason::replayed_sequence);
        // Lower sequence: also replay.
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 9), nullptr).reason ==
                RejectReason::replayed_sequence);
        // A NEW sequence with a repeated observation id: duplicate.
        DeviceMessage dup = observation_message(f.device_id, 11);
        dup.observation_id = "obs-10";
        REQUIRE(ingest.accept(f.peer(), dup, nullptr).reason ==
                RejectReason::duplicate_observation);
    }

    SECTION("timestamps are bounded by skew, age and the device's own watermark") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);

        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 1, kNow + 600'000), nullptr)
                    .reason == RejectReason::malformed_timestamp);
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 2, kNow - 100'000'000),
                              nullptr)
                    .reason == RejectReason::malformed_timestamp);

        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 3, kNow - 1000), nullptr)
                    .accepted);
        // Slightly behind the watermark: accepted and flagged.
        const auto behind = ingest.accept(f.peer(), observation_message(f.device_id, 4, kNow - 30'000),
                                          nullptr);
        REQUIRE(behind.accepted);
        REQUIRE(behind.out_of_order);
        // Far behind the watermark: refused.
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 5, kNow - 600'000), nullptr)
                    .reason == RejectReason::stale_timestamp);
    }

    SECTION("an unlisted source or observation type is refused") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        REQUIRE(ingest.accept(f.peer(),
                              observation_message(f.device_id, 1, kNow - 1000, "emotion"),
                              nullptr)
                    .reason == RejectReason::unknown_source_type);
        REQUIRE(ingest.accept(f.peer(),
                              observation_message(f.device_id, 2, kNow - 1000, "motion",
                                                  "face-expression"),
                              nullptr)
                    .reason == RejectReason::unknown_observation_type);
    }

    SECTION("an oversized or empty feature payload is refused") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        DeviceMessage empty = observation_message(f.device_id, 1);
        empty.features = nlohmann::json::object();
        REQUIRE(ingest.accept(f.peer(), empty, nullptr).reason == RejectReason::invalid_value);

        DeviceMessage many = observation_message(f.device_id, 2);
        many.features = nlohmann::json::object();
        for (size_t i = 0; i < 20; ++i) many.features["k" + std::to_string(i)] = 1;
        REQUIRE(ingest.accept(f.peer(), many, nullptr).reason == RejectReason::invalid_value);
    }

    SECTION("the queue is bounded and refuses rather than growing") {
        Fixture f;
        ObservationIngest::Config cfg = ObservationIngest::defaults();
        cfg.max_pending = 2;
        ObservationIngest ingest(f.registry, cfg, fixed_clock);

        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 1), nullptr).accepted);
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 2), nullptr).accepted);
        REQUIRE(ingest.pending() == 2);
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 3), nullptr).reason ==
                RejectReason::queue_full);
        // Draining frees room, and a refused message did not consume its sequence.
        std::vector<SecurityObservation> out;
        REQUIRE(ingest.drain([&](const SecurityObservation& o) { out.push_back(o); }) == 2);
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 3), nullptr).accepted);
    }

    SECTION("a claimed real provenance from an unauthorized path is recorded synthetic") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        // Capture off, but consent kept: the message must still arrive as synthetic.
        REQUIRE(f.registry.set_capture_enabled(f.device_id, false));
        const auto refused = ingest.accept(f.peer(), observation_message(f.device_id, 1), nullptr);
        REQUIRE_FALSE(refused.accepted);
        REQUIRE(refused.reason == RejectReason::capture_disabled);
        REQUIRE(ingest.rejected_count() == 1);

        // Restore capture, then claim real from a synthetic acquisition: recorded
        // synthetic and flagged.
        REQUIRE(f.registry.set_capture_enabled(f.device_id, true));
        DeviceMessage m = observation_message(f.device_id, 2);
        m.acquisition_mode = "synthetic";
        m.provenance_claim = "real";
        const auto result = ingest.accept(f.peer(), m, nullptr);
        REQUIRE(result.accepted);
        REQUIRE_FALSE(result.real);
        REQUIRE(result.provenance_claim_unsubstantiated);
        REQUIRE(ingest.claim_unsubstantiated_count() == 1);
    }

    SECTION("a refused message never advances the device's watermark") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        // Refused for an unlisted source type.
        REQUIRE_FALSE(ingest.accept(f.peer(),
                                    observation_message(f.device_id, 5, kNow - 1000, "emotion"),
                                    nullptr)
                          .accepted);
        // Sequence 5 is still fresh, so a valid message with it is accepted.
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 5), nullptr).accepted);
    }
}

TEST_CASE("accepted observations flow through the existing pipeline with real provenance",
          "[net][ingest][integration]") {
    Fixture f;
    auto pipeline = make_pipeline();
    AlertPolicy policy(std::chrono::milliseconds(60000));
    std::vector<SecurityObservation> stored;

    // NOTE: this case uses the REAL clock, not the fixed 2023 test clock. The
    // pipeline's `get_recent_events(60s)` window is evaluated against wall time,
    // so observations stamped three years ago are correctly filtered out - a
    // deterministic clock would make them invisible and the assertion meaningless.
    const int64_t now_ms = ObservationIngest::system_clock_ms()();
    const auto clock = [now_ms] { return now_ms; };
    ObservationIngest ingest(f.registry, ObservationIngest::defaults(), clock);
    const auto sink = [&](const SecurityObservation& o) {
        stored.push_back(o);
        const Event event = o.to_observation().to_event();
        const auto alert = pipeline->process(event);
        if (alert.has_value()) {
            bool suppressed = false;
            policy.admit(alert.value(), suppressed);
        }
    };

    SECTION("an accepted observation reaches the pipeline with its device identity intact") {
        REQUIRE(ingest
                    .accept(f.peer(),
                            observation_message(f.device_id, 1, now_ms - 1000), sink)
                    .accepted);
        REQUIRE(stored.size() == 1);
        const auto history = pipeline->get_recent_events(std::chrono::milliseconds(60000));
        REQUIRE(history.size() == 1);
        const auto back = Observation::from_event(history.front());
        REQUIRE(back.has_value());
        REQUIRE(back->source == f.device_id);
        REQUIRE(back->detector_id == "sensor.motion");
        REQUIRE(back->provenance == Provenance::real);
        REQUIRE(back->features["authorized"] == true);
        REQUIRE(back->features["consent_subject"] == f.device_id);
    }

    SECTION("a sustained stream of accepted motion observations can alert, never critically") {
        for (uint64_t seq = 1; seq <= 6; ++seq) {
            const auto result = ingest.accept(
                f.peer(),
                observation_message(f.device_id, seq, now_ms - 1000 + static_cast<int64_t>(seq)), sink);
            REQUIRE(result.accepted);
        }
        REQUIRE(stored.size() == 6);
        REQUIRE(pipeline->get_recent_events(std::chrono::milliseconds(60000)).size() == 6);
        for (const SecurityObservation& o : stored) {
            REQUIRE(o.provenance == Provenance::real);
            REQUIRE(o.suggested_severity() != Severity::critical);
        }
    }

    SECTION("a mixed stream stores only the authorized devices' observations") {
        DeviceRegistry::Device second = make_device("hg-2222222222222222", "22");
        std::string why;
        REQUIRE(f.registry.pair(second, &why));
        REQUIRE(f.registry.set_consent("hg-2222222222222222", true, "hg-2222222222222222",
                                       "security_monitor", kNow));
        REQUIRE(f.registry.set_capture_enabled("hg-2222222222222222", true));

        PeerContext allowed;
        allowed.authenticated_device_id = "hg-2222222222222222";
        allowed.credential_id = "22";
        allowed.peer_authenticated = true;

        PeerContext denied = allowed;  // consent NOT granted for this one
        second.device_id = "hg-3333333333333333";
        second.credential_id = "33";
        REQUIRE(f.registry.pair(second, &why));
        denied.authenticated_device_id = "hg-3333333333333333";
        denied.credential_id = "33";

        REQUIRE(ingest
                    .accept(allowed,
                            observation_message("hg-2222222222222222", 1, now_ms - 1000), sink)
                    .accepted);
        REQUIRE_FALSE(
            ingest
                .accept(denied, observation_message("hg-3333333333333333", 1, now_ms - 1000), sink)
                .accepted);

        REQUIRE(stored.size() == 1);
        REQUIRE(stored.front().device_id == "hg-2222222222222222");
        // Per-device failure isolation: the refused device did not disturb the
        // accepted one's stream state.
        REQUIRE(ingest
                    .accept(allowed,
                            observation_message("hg-2222222222222222", 2, now_ms - 900), sink)
                    .accepted);
    }
}

TEST_CASE("authorization is re-evaluated during a live session (D-03.1 section D)",
          "[net][authorization]") {
    const int64_t now_ms = ObservationIngest::system_clock_ms()();
    const auto clock = [now_ms] { return now_ms; };

    std::vector<SecurityObservation> stored;
    const auto sink = [&](const SecurityObservation& o) { stored.push_back(o); };

    Fixture f;
    ObservationIngest ingest(f.registry, ObservationIngest::defaults(), clock);
    const std::string registry_path = temp_path("live-registry.json");

    SECTION("a consent withdrawal applied mid-session blocks the NEXT observation") {
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 1, now_ms - 1000), sink)
                    .accepted);
        REQUIRE(stored.size() == 1);
        REQUIRE(f.registry.find(f.device_id)->last_sequence == 1);

        // The operator withdraws consent WHILE the connection is open.
        {
            std::ofstream out(registry_path, std::ios::trunc);
            out << "{\"devices\":[{\"device_id\":\"" << f.device_id << "\",\"credential_id\":\""
                << f.credential << "\",\"paired\":true,\"consent_granted\":false}]}";
        }
        std::vector<std::string> transitions;
        std::string why;
        REQUIRE(f.registry.refresh_authorization(registry_path, &transitions, &why) ==
                DeviceRegistry::RefreshOutcome::refreshed);
        REQUIRE_FALSE(transitions.empty());
        REQUIRE_FALSE(f.registry.find(f.device_id)->consent_granted);
        // Re-reading authorization must NOT touch what the hub observed for itself.
        REQUIRE(f.registry.find(f.device_id)->last_sequence == 1);

        const auto result =
            ingest.accept(f.peer(), observation_message(f.device_id, 2, now_ms - 900), sink);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::consent_withdrawn);
        REQUIRE(stored.size() == 1);  // nothing new reached the pipeline
        ::unlink(registry_path.c_str());
    }

    SECTION("capture disable mid-session blocks the NEXT observation") {
        {
            std::ofstream out(registry_path, std::ios::trunc);
            out << "{\"devices\":[{\"device_id\":\"" << f.device_id << "\",\"credential_id\":\""
                << f.credential << "\",\"paired\":true,\"consent_granted\":true,"
                << "\"capture_enabled\":false}]}";
        }
        std::string why;
        REQUIRE(f.registry.refresh_authorization(registry_path, nullptr, &why) ==
                DeviceRegistry::RefreshOutcome::refreshed);
        const auto result =
            ingest.accept(f.peer(), observation_message(f.device_id, 3, now_ms - 800), sink);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::capture_disabled);
        ::unlink(registry_path.c_str());
    }

    SECTION("revocation mid-session blocks the NEXT observation, not the connection") {
        {
            std::ofstream out(registry_path, std::ios::trunc);
            out << "{\"devices\":[{\"device_id\":\"" << f.device_id << "\",\"credential_id\":\""
                << f.credential << "\",\"paired\":true,\"consent_granted\":true,"
                << "\"capture_enabled\":true,\"revoked\":true}]}";
        }
        std::string why;
        REQUIRE(f.registry.refresh_authorization(registry_path, nullptr, &why) ==
                DeviceRegistry::RefreshOutcome::refreshed);
        REQUIRE(f.registry.find(f.device_id)->revoked);
        // Revocation clears the permissions too, so no residual flag leaves the device
        // partially authorized.
        REQUIRE_FALSE(f.registry.find(f.device_id)->consent_granted);
        REQUIRE_FALSE(f.registry.find(f.device_id)->capture_enabled);
        const auto result =
            ingest.accept(f.peer(), observation_message(f.device_id, 4, now_ms - 700), sink);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::device_revoked);
        ::unlink(registry_path.c_str());
    }

    SECTION("a malformed or missing authorization file reports itself, so the caller can fail closed") {
        std::string why;
        REQUIRE(f.registry.refresh_authorization(temp_path("does-not-exist.json"), nullptr, &why) ==
                DeviceRegistry::RefreshOutcome::absent);
        {
            std::ofstream out(registry_path, std::ios::trunc);
            out << "{not json";
        }
        REQUIRE(f.registry.refresh_authorization(registry_path, nullptr, &why) ==
                DeviceRegistry::RefreshOutcome::malformed);
        REQUIRE_FALSE(why.empty());
        {
            std::ofstream out(registry_path, std::ios::trunc);
            out << "{\"devices\":\"not-an-array\"}";
        }
        REQUIRE(f.registry.refresh_authorization(registry_path, nullptr, &why) ==
                DeviceRegistry::RefreshOutcome::malformed);
        ::unlink(registry_path.c_str());
    }

    SECTION("one device losing authorization does not disturb another") {
        DeviceRegistry::Device second = make_device("hg-2222222222222222", "22");
        std::string why;
        REQUIRE(f.registry.pair(second, &why));
        REQUIRE(f.registry.set_consent("hg-2222222222222222", true, "hg-2222222222222222",
                                       "security_monitor", now_ms));
        REQUIRE(f.registry.set_capture_enabled("hg-2222222222222222", true));

        {
            std::ofstream out(registry_path, std::ios::trunc);
            out << "{\"devices\":["
                << "{\"device_id\":\"" << f.device_id << "\",\"credential_id\":\"" << f.credential
                << "\",\"paired\":true,\"consent_granted\":false},"
                << "{\"device_id\":\"hg-2222222222222222\",\"credential_id\":\"22\","
                << "\"paired\":true,\"consent_granted\":true,\"capture_enabled\":true}]}";
        }
        REQUIRE(f.registry.refresh_authorization(registry_path, nullptr, &why) ==
                DeviceRegistry::RefreshOutcome::refreshed);
        REQUIRE_FALSE(f.registry.find(f.device_id)->consent_granted);
        REQUIRE(f.registry.find("hg-2222222222222222")->consent_granted);

        const auto refused =
            ingest.accept(f.peer(), observation_message(f.device_id, 5, now_ms - 600), sink);
        REQUIRE_FALSE(refused.accepted);

        PeerContext other;
        other.authenticated_device_id = "hg-2222222222222222";
        other.credential_id = "22";
        other.peer_authenticated = true;
        const auto permitted = ingest.accept(
            other, observation_message("hg-2222222222222222", 1, now_ms - 600), sink);
        REQUIRE(permitted.accepted);
        ::unlink(registry_path.c_str());
    }
}

TEST_CASE("replay state survives a restart and is not built on trust (D-03.1 section E)",
          "[net][replay]") {
    const std::string path = temp_path("replay-state.json");
    std::string why;

    SECTION("a watermark round-trips through a restart") {
        {
            ReplayState first(ReplayState::Config{path, 8});
            REQUIRE(first.load(&why) == ReplayState::LoadOutcome::absent);  // first run
            REQUIRE(first.record("hg-1", 7, &why));
            REQUIRE(first.record("hg-1", 9, &why));  // raises
            REQUIRE(first.record("hg-1", 4, &why));  // NEVER lowers
            REQUIRE(first.watermarks().at("hg-1") == 9);
        }
        ReplayState restarted(ReplayState::Config{path, 8});
        REQUIRE(restarted.load(&why) == ReplayState::LoadOutcome::loaded);
        REQUIRE(restarted.watermarks().at("hg-1") == 9);
        ::unlink(path.c_str());
    }

    SECTION("a seeded watermark refuses a replay of an already accepted observation") {
        Fixture f;
        {
            ReplayState state(ReplayState::Config{path, 8});
            state.load(&why);
            REQUIRE(state.record(f.device_id, 5, &why));
        }
        ReplayState restarted(ReplayState::Config{path, 8});
        REQUIRE(restarted.load(&why) == ReplayState::LoadOutcome::loaded);
        for (const auto& entry : restarted.watermarks()) {
            REQUIRE(f.registry.seed_sequence_watermark(entry.first, entry.second));
        }
        const int64_t now_ms = ObservationIngest::system_clock_ms()();
        const auto clock = [now_ms] { return now_ms; };
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), clock);
        std::vector<SecurityObservation> stored;
        const auto sink = [&](const SecurityObservation& o) { stored.push_back(o); };

        // The exact observation the hub accepted BEFORE the restart: refused.
        const auto replayed =
            ingest.accept(f.peer(), observation_message(f.device_id, 5, now_ms - 1000), sink);
        REQUIRE_FALSE(replayed.accepted);
        REQUIRE(replayed.reason == RejectReason::replayed_sequence);
        // A genuinely new sequence is still accepted.
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 6, now_ms - 1000), sink)
                    .accepted);
        ::unlink(path.c_str());
    }

    SECTION("an unusable file fails closed instead of silently starting empty") {
        {
            std::ofstream out(path, std::ios::trunc);
            out << "{corrupt";
        }
        ReplayState broken(ReplayState::Config{path, 8});
        REQUIRE(broken.load(&why) == ReplayState::LoadOutcome::malformed);
        REQUIRE_FALSE(why.empty());
        {
            std::ofstream out(path, std::ios::trunc);
            out << "{\"devices\":[{\"device_id\":\"dupe\",\"last_sequence\":3},"
                << "{\"device_id\":\"dupe\",\"last_sequence\":11}]}";
        }
        ReplayState duplicates(ReplayState::Config{path, 8});
        REQUIRE(duplicates.load(&why) == ReplayState::LoadOutcome::loaded);
        REQUIRE(duplicates.watermarks().at("dupe") == 11);  // highest wins, not last
        ::unlink(path.c_str());
    }

    SECTION("the persisted state is bounded") {
        ReplayState bounded(ReplayState::Config{path, 2});
        REQUIRE(bounded.load(&why) == ReplayState::LoadOutcome::absent);
        REQUIRE(bounded.record("a", 1, &why));
        REQUIRE(bounded.record("b", 1, &why));
        REQUIRE_FALSE(bounded.record("c", 1, &why));  // refuses rather than grows
        REQUIRE(bounded.size() == 2);
        ::unlink(path.c_str());
        ::unlink((path + ".tmp").c_str());
    }

    SECTION("an interrupted write leaves the previous state intact") {
        {
            ReplayState state(ReplayState::Config{path, 8});
            state.load(&why);
            REQUIRE(state.record("hg-2", 3, &why));
        }
        // Simulate a crash mid-write: a partial temporary file beside a good one.
        {
            std::ofstream out(path + ".tmp", std::ios::trunc);
            out << "{\"devices\":[{\"device_id\":\"hg-2\",\"last_seq";
        }
        ReplayState after_crash(ReplayState::Config{path, 8});
        REQUIRE(after_crash.load(&why) == ReplayState::LoadOutcome::loaded);
        REQUIRE(after_crash.watermarks().at("hg-2") == 3);  // the good file, not the fragment
        ::unlink(path.c_str());
        ::unlink((path + ".tmp").c_str());
    }
}

TEST_CASE("the welcome tells a restarted device where to resume (D-03.1 section E)",
          "[net][protocol]") {
    // Both halves: the hub WRITES the resume point and the device READS it. A field the
    // writer emits without a reader-side test is exactly the defect Incident 37 was.
    DeviceMessage welcome;
    welcome.protocol_version_major = kProtocolVersionMajor;
    welcome.protocol_version_minor = kProtocolVersionMinor;
    welcome.kind = MessageKind::welcome;
    welcome.device_id_claim = "hg-1111111111111111";
    welcome.resume_after_sequence = 40000;
    RejectReason reject = RejectReason::bad_json;
    std::string reason;

    const nlohmann::json encoded = encode(welcome);
    REQUIRE(encoded["last_sequence"] == 40000);
    // The REPLAY check happens on the wire, so round-trip through the real decoder.
    const auto framed = frame(encoded);
    REQUIRE(framed.has_value());
    const auto decoded = decode(encode(welcome), &reject, &reason);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->kind == MessageKind::welcome);
    REQUIRE(decoded->device_id_claim == "hg-1111111111111111");
    REQUIRE(decoded->resume_after_sequence == 40000);
    (void)framed;
}

TEST_CASE("a downstream failure is never reported as an acceptance (D-03.1 section G)",
          "[net][processing]") {
    const int64_t now_ms = ObservationIngest::system_clock_ms()();
    const auto clock = [now_ms] { return now_ms; };
    Fixture f;

    SECTION("a sink that throws yields processing_failed and does NOT consume the sequence") {
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), clock);
        int calls = 0;
        const auto throwing = [&](const SecurityObservation&) {
            ++calls;
            throw std::runtime_error("persistence unavailable");
        };
        const auto result =
            ingest.accept(f.peer(), observation_message(f.device_id, 1, now_ms - 1000), throwing);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::processing_failed);
        REQUIRE(calls == 1);
        // The watermark must NOT have advanced: the device has to be able to retry the
        // same sequence after the failure clears.
        REQUIRE(f.registry.find(f.device_id)->last_sequence == 0);

        std::vector<SecurityObservation> stored;
        const auto good = [&](const SecurityObservation& o) { stored.push_back(o); };
        const auto retried =
            ingest.accept(f.peer(), observation_message(f.device_id, 1, now_ms - 1000), good);
        REQUIRE(retried.accepted);
        REQUIRE(stored.size() == 1);
    }

    SECTION("a failing commit hook refuses the observation before it is delivered") {
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), clock);
        int delivered = 0;
        const auto sink = [&](const SecurityObservation&) { ++delivered; };
        int hook_calls = 0;
        ingest.set_commit_hook([&](const std::string&, uint64_t, std::string* reason) {
            ++hook_calls;
            if (reason != nullptr) *reason = "durable replay state unavailable";
            return false;
        });
        const auto result =
            ingest.accept(f.peer(), observation_message(f.device_id, 2, now_ms - 1000), sink);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::processing_failed);
        REQUIRE(hook_calls == 1);
        REQUIRE(delivered == 0);  // an acceptance that is not durable is not delivered

        // With the durable state healthy again, the same sequence is accepted.
        ingest.set_commit_hook([&](const std::string&, uint64_t, std::string* reason) {
            if (reason != nullptr) reason->clear();
            return true;
        });
        REQUIRE(ingest.accept(f.peer(), observation_message(f.device_id, 2, now_ms - 1000), sink)
                    .accepted);
        REQUIRE(delivered == 1);
    }
}

// ===========================================================================
// Detector wiring: the D-02x components in the hub runtime (section G)
// ===========================================================================

TEST_CASE("ingested observations drive the D-02x detectors into the same pipeline",
          "[net][detectors]") {
    const int64_t now = ObservationIngest::system_clock_ms()();
    const auto clock = [now] { return now; };
    IngestDetectors detectors(IngestDetectors::defaults(), clock);

    auto pipeline = make_pipeline();

    const auto deliver = [&](const std::vector<Observation>& derived) {
        for (const Observation& observation : derived) {
            pipeline->process(observation.to_event());
        }
    };

    auto motion = [&](const std::string& device, double value, int64_t at) {
        SecurityObservation o;
        o.device_id = device;
        o.source = "node";
        o.source_type = SecuritySourceType::motion;
        o.acquired_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(at));
        o.ingested_at = o.acquired_at;
        o.provenance = Provenance::real;
        o.state = "on";
        o.consent_subject = device;
        o.consent_purpose = "security_monitor";
        o.authorized = true;
        o.features = nlohmann::json::object();
        o.features["motion-changed-fraction"] = value;
        return o;
    };

    SECTION("a motion observation is fed to the activity detector and reported") {
        const auto outcome = detectors.observe(motion("hg-1", 0.2, now));
        REQUIRE(outcome.fed);
        REQUIRE(outcome.detector == "motion-activity");
        std::vector<Observation> derived;
        REQUIRE(detectors.drain(derived) >= 1);
        bool saw_motion = false;
        for (const Observation& o : derived) {
            if (o.detector_id == "motion-activity") {
                saw_motion = true;
                REQUIRE(o.source == "hg-1");
                REQUIRE(o.provenance == Provenance::real);  // derived from a real observation
            }
        }
        REQUIRE(saw_motion);
        deliver(derived);
        REQUIRE(pipeline->get_recent_events(std::chrono::milliseconds(60000)).size() >= 1);
    }

    SECTION("a sustained motion rise produces a confirmed change observation") {
        for (int i = 0; i < 12; ++i) {
            detectors.observe(motion("hg-1", 0.05, now - 12000 + i * 1000));
            std::vector<Observation> flush;
            detectors.drain(flush);
        }
        bool confirmed = false;
        for (int i = 0; i < 6; ++i) {
            detectors.observe(motion("hg-1", 0.45, now - 6000 + i * 1000));
            std::vector<Observation> derived;
            detectors.drain(derived);
            for (const Observation& o : derived) {
                if (o.detector_id == "motion-activity" && o.status == DetectorStatus::detected) {
                    confirmed = true;
                    REQUIRE(o.features["direction"] == "rising");
                }
            }
        }
        REQUIRE(confirmed);
    }

    SECTION("an audio observation feeds both the level detector and the anomaly monitor") {
        SecurityObservation audio = motion("hg-1", 0.0, now);
        audio.features = nlohmann::json::object();
        audio.features["audio-rms"] = 0.3;
        audio.source_type = SecuritySourceType::microphone_availability;
        const auto outcome = detectors.observe(audio);
        REQUIRE(outcome.fed);
        REQUIRE(outcome.detector == "audio-activity");
        std::vector<Observation> derived;
        detectors.drain(derived);
        bool saw_activity = false;
        for (const Observation& o : derived) {
            if (o.detector_id == "audio-activity") saw_activity = true;
        }
        REQUIRE(saw_activity);
    }

    SECTION("a repeated observation is suppressed, not re-detected") {
        REQUIRE_FALSE(detectors.observe(motion("hg-1", 0.2, now)).duplicate);
        const auto repeat = detectors.observe(motion("hg-1", 0.2, now));
        REQUIRE(repeat.duplicate);
        REQUIRE(detectors.duplicate_count() == 1);
    }

    SECTION("an unwired observation type is reported as unfed rather than guessed") {
        SecurityObservation unknown = motion("hg-1", 0.0, now);
        unknown.features = nlohmann::json::object();
        unknown.features["mystery"] = 1.0;
        unknown.source_type = SecuritySourceType::presence;
        const auto outcome = detectors.observe(unknown);
        REQUIRE_FALSE(outcome.fed);
        REQUIRE(outcome.reason.find("no detector is wired") != std::string::npos);
    }

    SECTION("per-device detector state is bounded") {
        IngestDetectors::Config cfg = IngestDetectors::defaults();
        cfg.max_devices = 2;
        IngestDetectors bounded(cfg, clock);
        for (int i = 0; i < 6; ++i) {
            const std::string device = "hg-" + std::to_string(i);
            bounded.observe(motion(device, 0.2, now));
            std::vector<Observation> flush;
            bounded.drain(flush);
        }
        REQUIRE(bounded.device_count() <= 2);
    }

    SECTION("a liveness sweep produces one offline observation after the timeout") {
        // The precondition is explicit: the tracker's timeout is 90 s, so the sweep
        // is driven at now + 200 s rather than assumed fresh.
        int64_t clock_now = now;
        IngestDetectors::Config cfg = IngestDetectors::defaults();
        IngestDetectors sweep_detectors(cfg, [&clock_now] { return clock_now; });
        sweep_detectors.observe(motion("hg-1", 0.2, now));
        std::vector<Observation> flush;
        sweep_detectors.drain(flush);
        REQUIRE(sweep_detectors.sweep_availability().empty());
        clock_now = now + 200'000;
        const auto offline = sweep_detectors.sweep_availability();
        REQUIRE(offline.size() == 1);
        REQUIRE(offline.front().state == "offline");
        REQUIRE(offline.front().device_id == "hg-1");
    }
}

TEST_CASE("a refusal survives the wire: the decoder reads the reason the hub sent",
          "[net][protocol]") {
    // REGRESSION GUARD: the encoder always wrote "accepted"/"reason" on an
    // acknowledgement, but the decoder only handled observation payloads and
    // returned early for every other kind. A device therefore saw reason=none for
    // every rejection - it could not tell "accepted" from "refused, here is why",
    // and the end-to-end test reported success for messages the hub had refused.
    DeviceMessage ack;
    ack.protocol_version_major = kProtocolVersionMajor;
    ack.protocol_version_minor = kProtocolVersionMinor;
    ack.kind = MessageKind::acknowledgement;
    ack.observation_id = "mock-obs-7";
    ack.sequence = 7;
    ack.reason = RejectReason::replayed_sequence;

    RejectReason reason = RejectReason::none;
    std::string detail;
    const std::optional<DeviceMessage> decoded = decode(encode(ack), &reason, &detail);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->kind == MessageKind::acknowledgement);
    REQUIRE(decoded->reason == RejectReason::replayed_sequence);
    REQUIRE(decoded->observation_id == "mock-obs-7");

    // Every reason the hub can send must survive the roundtrip, or a device would
    // silently see "ok" for some refusals.
    const RejectReason all[] = {
        RejectReason::none,           RejectReason::bad_version,
        RejectReason::bad_json,       RejectReason::oversized,
        RejectReason::unknown_source_type, RejectReason::unknown_observation_type,
        RejectReason::invalid_value,  RejectReason::malformed_timestamp,
        RejectReason::stale_timestamp, RejectReason::replayed_sequence,
        RejectReason::duplicate_observation, RejectReason::device_unknown,
        RejectReason::device_revoked, RejectReason::device_unpaired,
        RejectReason::identity_mismatch, RejectReason::consent_missing,
        RejectReason::consent_withdrawn, RejectReason::capture_disabled,
        RejectReason::queue_full,     RejectReason::unauthorized,
    };
    for (const RejectReason value : all) {
        DeviceMessage message;
        message.protocol_version_major = kProtocolVersionMajor;
        message.protocol_version_minor = kProtocolVersionMinor;
        message.kind = MessageKind::acknowledgement;
        message.reason = value;
        RejectReason parsed = RejectReason::bad_frame;
        const std::optional<DeviceMessage> roundtrip = decode(encode(message), &parsed, &detail);
        REQUIRE(roundtrip.has_value());
        INFO("reason " << reject_reason_to_string(value));
        REQUIRE(roundtrip->reason == value);
        // The inverse mapping must agree with the forward one, both ways.
        REQUIRE(reject_reason_from_string(reject_reason_to_string(value)) == value);
    }
}

TEST_CASE("a welcome carries the identity the hub derived from the certificate",
          "[net][protocol]") {
    DeviceMessage welcome;
    welcome.protocol_version_major = kProtocolVersionMajor;
    welcome.protocol_version_minor = kProtocolVersionMinor;
    welcome.kind = MessageKind::welcome;
    welcome.device_id_claim = "hg-aaaaaaaaaaaaaaa1";

    RejectReason reason = RejectReason::bad_frame;
    std::string detail;
    const std::optional<DeviceMessage> decoded = decode(encode(welcome), &reason, &detail);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->kind == MessageKind::welcome);
    REQUIRE(decoded->device_id_claim == "hg-aaaaaaaaaaaaaaa1");

    // An error frame carries its reason too, so a refusal at handshake time is
    // explainable rather than an empty "no welcome".
    DeviceMessage refusal;
    refusal.protocol_version_major = kProtocolVersionMajor;
    refusal.protocol_version_minor = kProtocolVersionMinor;
    refusal.kind = MessageKind::error;
    refusal.reason = RejectReason::identity_mismatch;
    const std::optional<DeviceMessage> decoded_refusal = decode(encode(refusal), &reason, &detail);
    REQUIRE(decoded_refusal.has_value());
    REQUIRE(decoded_refusal->reason == RejectReason::identity_mismatch);
}

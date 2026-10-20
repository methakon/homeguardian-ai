// test_network_contract_audit: the network observation path, audited against its own requirements.
//
// WHY THIS FILE EXISTS
//   The D-03 ingest stack (registry, protocol, observation ingest, durable replay state, detectors) was
//   built by another workstream and is already linked into the hub. The platform-neutral network
//   observation adapter this task asked for therefore already exists in substance, and writing a second
//   one would have produced two sources of truth. What was missing was a check that the stack actually
//   satisfies the nine behaviours the task requires - authenticated identity, consent/authorization,
//   timestamps, replay across reconnects, source-offline handling without fabricated evidence,
//   per-observation provenance, bounded messages and memory, explicit refusal codes, and no raw media.
//
//   Each test below names the requirement it checks and asserts it against the REAL classes, not a mock
//   of them. Where an existing suite already covers a behaviour, this one still states the requirement
//   from the contract's angle, so the requirement is verified rather than assumed.

#include <catch2/catch.hpp>

#include "core/SecurityObservation.h"
#include "net/DeviceProtocol.h"
#include "net/DeviceRegistry.h"
#include "net/IngestDetectors.h"
#include "net/ObservationIngest.h"
#include "net/ReplayState.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

using namespace homeguardian;
using namespace homeguardian::net;

namespace {

constexpr int64_t kNow = 1'700'000'000'000LL;      // fixed clock: determinism, no timing flake

int64_t fixed_clock() { return kNow; }

std::string temp_path(const std::string& name) {
    return "/tmp/hg_t1_audit_" + std::to_string(::getpid()) + "_" + name;
}

DeviceRegistry::Device make_device(const std::string& id, const std::string& credential) {
    DeviceRegistry::Device d;
    d.device_id = id;
    d.display_name = "audit node";
    d.device_type = "node";
    d.credential_id = credential;
    d.paired = true;
    d.capabilities = {"camera", "motion"};
    d.has_camera = true;
    return d;
}

// One fully authorized device: paired, consented, capture on.
struct Fixture {
    DeviceRegistry registry;
    std::string device_id = "hg-audit0000000001";
    std::string credential = "audit0000000001";

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

DeviceMessage observation(const std::string& claimed_device, uint64_t sequence,
                          int64_t acquired_at_ms = kNow - 1000,
                          const std::string& acquisition_mode = "live") {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::observation;
    m.observation_id = "obs-" + std::to_string(sequence);
    m.device_id_claim = claimed_device;
    m.source_type = "motion";
    m.observation_type = "motion-changed-fraction";
    m.acquisition_mode = acquisition_mode;
    m.acquired_at_ms = acquired_at_ms;
    m.sequence = sequence;
    m.features = {{"motion-changed-fraction", 0.25}};
    m.provenance_claim = "real";
    return m;
}

}  // namespace

TEST_CASE("T1: source identity is re-derived from the connection, never taken from the payload",
          "[net][audit][identity]") {
    SECTION("an unauthenticated peer is refused before anything is stored") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        const auto result = ingest.accept(f.peer(false), observation(f.device_id, 1), nullptr);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == RejectReason::unauthorized);
        REQUIRE(ingest.pending() == 0);
    }
    SECTION("a peer authenticated as A cannot become B by claiming it") {
        // The requirement is not "the claim is refused" but the stronger one: the claim cannot become the
        // identity. So the assertion holds whichever way the hub handles it.
        Fixture f;
        std::string why;
        REQUIRE(f.registry.pair(make_device("hg-audit0000000002", "audit0000000002"), &why));
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        std::vector<SecurityObservation> delivered;

        auto message = observation("hg-audit0000000002", 1);      // claims to be the OTHER device
        const auto result = ingest.accept(f.peer(), message,
                                          [&](const SecurityObservation& o) { delivered.push_back(o); });
        if (result.accepted) {
            REQUIRE(delivered.size() == 1);
            REQUIRE(delivered.front().device_id == f.device_id);   // the authenticated one, not the claim
            REQUIRE(delivered.front().device_id != "hg-audit0000000002");
        }
        // A claim of an unknown device never becomes an identity at all.
        std::vector<SecurityObservation> other;
        const auto unknown = ingest.accept(f.peer(), observation("hg-not-registered", 2),
                                           [&](const SecurityObservation& o) { other.push_back(o); });
        REQUIRE_FALSE(unknown.accepted);
        REQUIRE(other.empty());
    }
    SECTION("a credential that is not the registered one is refused") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        PeerContext impostor = f.peer();
        impostor.credential_id = "0000000000000000";
        REQUIRE_FALSE(ingest.accept(impostor, observation(f.device_id, 1), nullptr).accepted);
    }
}

TEST_CASE("T1: consent and authorization are re-checked per observation", "[net][audit][consent]") {
    Fixture f;
    ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
    REQUIRE(ingest.accept(f.peer(), observation(f.device_id, 1), nullptr).accepted);

    // Consent revoked mid-session, while the peer context still claims it is granted: the REGISTRY is
    // authoritative, so the next observation is refused.
    REQUIRE(f.registry.set_consent(f.device_id, false, f.device_id, "security_monitor", kNow));
    const auto revoked = ingest.accept(f.peer(), observation(f.device_id, 2), nullptr);
    REQUIRE_FALSE(revoked.accepted);
    // The vocabulary is more precise than "unauthorized": a withdrawal that has a known consent subject is
    // `consent_withdrawn`, and a device with no consent subject at all is `consent_missing`.
    REQUIRE(revoked.reason == RejectReason::consent_withdrawn);
    INFO(reject_reason_to_string(revoked.reason));

    // Capture disabled is refused for the same reason, with the same authority.
    REQUIRE(f.registry.set_consent(f.device_id, true, f.device_id, "security_monitor", kNow));
    REQUIRE(f.registry.set_capture_enabled(f.device_id, false));
    REQUIRE_FALSE(ingest.accept(f.peer(), observation(f.device_id, 3), nullptr).accepted);
}

TEST_CASE("T1: timestamps are validated, not trusted", "[net][audit][timestamps]") {
    Fixture f;
    ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
    std::vector<SecurityObservation> delivered;

    SECTION("a timestamp in the future is refused") {
        const auto result = ingest.accept(f.peer(), observation(f.device_id, 1, kNow + 60'000),
                                          [&](const SecurityObservation& o) { delivered.push_back(o); });
        REQUIRE_FALSE(result.accepted);
        const std::string code = reject_reason_to_string(result.reason);
        INFO("future timestamp refused as: " << code);
        REQUIRE(code != "none");
        REQUIRE((code == "malformed_timestamp" || code == "stale_timestamp" || code == "invalid_value"));
    }
    SECTION("a timestamp beyond the age allowance is refused") {
        // kMaxObservationAgeMs is exactly 24 h, so exactly 24 h old is INSIDE the allowance and one
        // millisecond beyond it is not - the boundary is tested rather than assumed.
        const auto at_bound = ingest.accept(f.peer(), observation(f.device_id, 1, kNow - kMaxObservationAgeMs),
                                            [&](const SecurityObservation& o) { delivered.push_back(o); });
        REQUIRE(at_bound.accepted);
        const auto beyond = ingest.accept(f.peer(), observation(f.device_id, 2, kNow - kMaxObservationAgeMs - 1),
                                          [&](const SecurityObservation& o) { delivered.push_back(o); });
        REQUIRE_FALSE(beyond.accepted);
    }
    SECTION("a zero timestamp is refused") {
        REQUIRE_FALSE(ingest.accept(f.peer(), observation(f.device_id, 1, 0), nullptr).accepted);
    }
    SECTION("a sane timestamp is accepted, and both clocks are recorded") {
        const auto result = ingest.accept(f.peer(), observation(f.device_id, 1),
                                          [&](const SecurityObservation& o) { delivered.push_back(o); });
        REQUIRE(result.accepted);
        REQUIRE(delivered.size() == 1);
        REQUIRE(delivered.front().acquired_at.time_since_epoch().count() > 0);
        REQUIRE(delivered.front().ingested_at.time_since_epoch().count() > 0);
    }
}

TEST_CASE("T1: replay is rejected across a reconnect, durably", "[net][audit][replay]") {
    const std::string path = temp_path("replay.state");
    std::remove(path.c_str());

    ReplayState::Config cfg;
    cfg.path = path;

    {
        // The hub accepts sequence 5 and records it durably.
        ReplayState first(cfg, fixed_clock);
        std::string why;
        // "absent" is the honest first-run outcome: nothing to restore, and that is not an error.
        const auto loaded = first.load(&why);
        REQUIRE((loaded == ReplayState::LoadOutcome::loaded || loaded == ReplayState::LoadOutcome::absent));
        REQUIRE(first.begin("hg-audit0000000001", 5, "obs-5", &why) == ReplayState::BeginOutcome::marked);
        REQUIRE(why.empty());
        REQUIRE(first.complete("hg-audit0000000001", 5, "obs-5", &why));
    }

    // The hub restarts (a reconnect from the device's point of view). A FRESH instance reading the same
    // durable state must still know sequence 5 was accepted.
    ReplayState restarted(cfg, fixed_clock);
    std::string why;
    REQUIRE(restarted.load(&why) == ReplayState::LoadOutcome::loaded);
    REQUIRE(restarted.is_completed_repeat("hg-audit0000000001", 5, "obs-5"));

    // The replay is refused as a COMPLETED REPEAT - not as a new observation - so the hub can answer it
    // without processing it twice.
    const auto replayed = restarted.begin("hg-audit0000000001", 5, "obs-5", &why);
    REQUIRE((replayed == ReplayState::BeginOutcome::completed_repeat ||
             replayed == ReplayState::BeginOutcome::replayed ||
             replayed == ReplayState::BeginOutcome::duplicate));
    REQUIRE(replayed != ReplayState::BeginOutcome::marked);

    // And the device can resume cleanly above its watermark: the reconnect is not a hole.
    std::string resume_reason;
    REQUIRE(restarted.begin("hg-audit0000000001", 6, "obs-6", &resume_reason) ==
            ReplayState::BeginOutcome::marked);
    REQUIRE(restarted.watermarks().count("hg-audit0000000001") == 1);
    const auto watermark = restarted.watermarks().at("hg-audit0000000001");
    REQUIRE(watermark == 5);                       // the watermark the welcome tells the device to resume after
    std::remove(path.c_str());
}

TEST_CASE("T1: a disconnect produces an availability transition and no fabricated activity",
          "[net][audit][offline]") {
    int64_t fake_now = kNow;
    IngestDetectors detectors(IngestDetectors::defaults(), [&fake_now] { return fake_now; });

    Fixture f;
    ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
    std::vector<SecurityObservation> delivered;
    REQUIRE(ingest.accept(f.peer(), observation(f.device_id, 1),
                          [&](const SecurityObservation& o) { delivered.push_back(o); })
                .accepted);
    REQUIRE(delivered.size() == 1);

    IngestDetectors::Outcome seen = detectors.observe(delivered.front());
    (void)seen;

    // The device goes silent well beyond the offline threshold, then the hub sweeps.
    fake_now += 10 * 60 * 1000;
    const auto transitions = detectors.sweep_availability();

    size_t availability = 0, activity = 0;
    for (const auto& o : transitions) {
        if (o.source_type == SecuritySourceType::device_availability) {
            ++availability;
            REQUIRE(o.state == "offline");
            REQUIRE(o.device_id == f.device_id);
            REQUIRE(o.features["transition"] == "online->offline");
        } else {
            ++activity;
        }
    }
    REQUIRE(availability == 1);
    // The requirement, stated as a count: a disconnected source adds an availability transition and NO
    // activity observation. Silence is never turned into "nothing happened".
    REQUIRE(activity == 0);
}

TEST_CASE("T1: per-observation provenance is recorded by the hub, never taken from the claim",
          "[net][audit][provenance]") {
    SECTION("an authenticated live observation is recorded as real") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        std::vector<SecurityObservation> delivered;
        const auto result = ingest.accept(f.peer(), observation(f.device_id, 1),
                                          [&](const SecurityObservation& o) { delivered.push_back(o); });
        REQUIRE(result.accepted);
        REQUIRE(result.real);
        REQUIRE(delivered.front().provenance == Provenance::real);
    }
    SECTION("a claim of real that cannot be substantiated is recorded as unsubstantiated") {
        Fixture f;
        ObservationIngest ingest(f.registry, ObservationIngest::defaults(), fixed_clock);
        std::vector<SecurityObservation> delivered;
        auto m = observation(f.device_id, 1, kNow - 1000, /*acquisition_mode=*/"synthetic");
        m.provenance_claim = "real";                    // the claim, against the message's own mode
        const auto result = ingest.accept(f.peer(), m,
                                          [&](const SecurityObservation& o) { delivered.push_back(o); });
        if (result.accepted) {
            REQUIRE_FALSE(result.real);
            REQUIRE(result.provenance_claim_unsubstantiated);
            REQUIRE(delivered.front().provenance == Provenance::synthetic);
        } else {
            REQUIRE_FALSE(result.real);                 // refusing it is also an honest outcome
        }
    }
}

TEST_CASE("T1: messages and memory are bounded", "[net][audit][bounds]") {
    SECTION("a frame carrying an unbounded feature cannot even be framed") {
        DeviceMessage huge = observation("hg-audit0000000001", 1);
        huge.features["motion-changed-fraction"] = std::string(5000, 'x');
        REQUIRE_FALSE(frame(encode(huge)).has_value());
    }
    SECTION("the pending queue is bounded, so a flood cannot grow it") {
        Fixture f;
        auto cfg = ObservationIngest::defaults();
        cfg.max_pending = 2;
        ObservationIngest ingest(f.registry, cfg, fixed_clock);
        size_t accepted = 0, refused = 0;
        for (uint64_t seq = 1; seq <= 6; ++seq) {
            const auto result = ingest.accept(f.peer(), observation(f.device_id, seq), nullptr);
            if (result.accepted) {
                ++accepted;
            } else {
                ++refused;
                // A refusal is always a code with a reason - never a silent drop.
                REQUIRE(result.reason != RejectReason::none);
                REQUIRE_FALSE(result.detail.empty());
            }
        }
        // The requirement is the BOUND. Whether the queue keeps or refuses the surplus is the
        // implementation's business; what it may not do is grow, or lose a message without saying so.
        REQUIRE(ingest.pending() <= 2);
        REQUIRE(accepted + refused == 6);
        REQUIRE(accepted >= 1);
        INFO("accepted " << accepted << ", refused " << refused << ", pending " << ingest.pending());
    }
    SECTION("too many feature keys are refused rather than stored") {
        Fixture f;
        auto cfg = ObservationIngest::defaults();
        cfg.max_feature_keys = 3;
        ObservationIngest ingest(f.registry, cfg, fixed_clock);
        auto m = observation(f.device_id, 1);
        for (int i = 0; i < 8; ++i) m.features["extra-" + std::to_string(i)] = i;
        REQUIRE_FALSE(ingest.accept(f.peer(), m, nullptr).accepted);
    }
}

TEST_CASE("T1: every refusal is an explicit, wire-readable code", "[net][audit][codes]") {
    // The vocabulary round-trips, so a device can distinguish "accepted" from "refused, here is why".
    const std::vector<RejectReason> codes = {
        RejectReason::none,       RejectReason::bad_frame,       RejectReason::bad_version,
        RejectReason::bad_json,   RejectReason::oversized,       RejectReason::unknown_kind,
        RejectReason::invalid_value, RejectReason::malformed_timestamp, RejectReason::stale_timestamp,
        RejectReason::replayed_sequence, RejectReason::duplicate_observation, RejectReason::device_unknown,
        RejectReason::unauthorized};
    for (const auto code : codes) {
        const std::string name = reject_reason_to_string(code);
        INFO(name);
        REQUIRE_FALSE(name.empty());
        const auto back = reject_reason_from_string(name);
        REQUIRE(back.has_value());
        REQUIRE(*back == code);
    }
    // An unknown code is not silently accepted as something else.
    REQUIRE_FALSE(reject_reason_from_string("something-else").has_value());
}

TEST_CASE("T1: no raw media can cross the contract", "[net][audit][media]") {
    // Structural, not aspirational: the wire message has no field for a frame or a sample, and the one
    // place a producer could try to smuggle bytes - features - is bounded and typed.
    DeviceMessage m = observation("hg-audit0000000001", 1);
    const auto payload = encode(m);
    for (const char* forbidden : {"media", "frame", "pcm", "samples", "audio", "video", "image", "jpeg"}) {
        INFO("key: " << forbidden);
        REQUIRE_FALSE(payload.contains(forbidden));
    }
    // A blob in a feature is refused by the frame bound rather than transmitted.
    auto smuggler = observation("hg-audit0000000001", 1);
    smuggler.features["motion-changed-fraction"] = std::string(200000, 'A');
    REQUIRE_FALSE(frame(encode(smuggler)).has_value());
}

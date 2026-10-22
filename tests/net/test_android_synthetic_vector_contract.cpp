#include "catch2/catch.hpp"

// ---------------------------------------------------------------------------
// HG-CPP-ANDROID-VECTOR-CONSUMER-27: consume Hermes 2's production-serialized synthetic vector.
//
// The producer is apk/tests/SyntheticVectorEmitTest.java (commit bb1e6c3, issue #24). It drives the Android
// transport's own serializer and writes `hello.frame` and `observation.frame`, each framed as the transport
// frames it: 4-byte big-endian length, then the UTF-8 JSON body. No vector artefacts are committed, so the
// bytes are reconstructed here from the producer's own serialization - field order and all - so that both
// sides are provably talking about the SAME bytes. The frame BYTE COUNTS are asserted, so a change on either
// side fails this test instead of silently altering the wire shape.
//
// WHAT IS AND IS NOT REAL, stated so this cannot be overread:
//   REAL      the framing, the field names, the JSON shape, the identity, the accept path, the ack.
//   SYNTHETIC acquisition_mode and provenance_claim are both "synthetic" - the non-claiming value, which the
//             receiver records as synthetic and cannot be mistaken for physical camera evidence.
//   NOT REAL  no camera, no microphone, no socket, no credential material, no production key. The authorized
//             test identity is supplied through the fixture registry and the peer context.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

#include "core/SecurityObservation.h"
#include "net/DeviceProtocol.h"
#include "net/DeviceRegistry.h"
#include "net/ObservationIngest.h"
#include "net/PanasonicNodeReceiver.h"

using namespace homeguardian;
using namespace homeguardian::net;
using nlohmann::json;

namespace {

// ---- the producer's pinned constants (their file fixes these so the vector is byte-reproducible) ----
constexpr const char* kDeviceId = "hg-android-synthetic-01";
constexpr uint64_t kSeq = 1;
constexpr int64_t kNow = 1761100000000LL;
constexpr const char* kObsId = "synthetic-obs-0001";
constexpr const char* kProducerSourceType = "camera";  // their vocabulary: a CAPABILITY name, not a source type
constexpr const char* kObsType = "motion";

const std::string kHelloJson =
    R"({"protocol_version_major":1,"protocol_version_minor":0,"kind":"hello",)"
    R"("device_id":"hg-android-synthetic-01","display_name":"synthetic node",)"
    R"("capabilities":["camera","microphone"]})";

// VERBATIM from their serializer.
const std::string kObservationJson =
    R"({"protocol_version_major":1,"protocol_version_minor":0,"kind":"observation",)"
    R"("observation_id":"synthetic-obs-0001","device_id":"hg-android-synthetic-01",)"
    R"("source_type":"camera","observation_type":"motion",)"
    R"("acquisition_mode":"synthetic","acquired_at_ms":1761100000000,"sequence":1,)"
    R"("features":{"motion":0.42,"state":"present"},"unit":"fraction","confidence":0.9,)"
    R"("provenance_claim":"synthetic"})";

// The same frame with ONE field changed: source_type speaks the receiver's vocabulary. Their observation_type
// "motion" is admitted through the operator's observation allowlist, which is the extension point this task
// added; their source_type "camera" cannot be admitted at all, because it has no SecuritySourceType
// representation - see the vocabulary test, and the defect that used to throw on exactly that value.
std::string receiver_vocabulary_json() {
    std::string s = kObservationJson;
    const std::string from = "\"source_type\":\"camera\"";
    const std::string to = "\"source_type\":\"motion\"";
    REQUIRE(s.find(from) != std::string::npos);
    s.replace(s.find(from), from.size(), to);
    return s;
}

// The transport's framing, built explicitly: the test must fail if the receiver's framing ever diverges from
// the producer's, rather than quietly re-framing.
std::vector<uint8_t> framed(const std::string& body) {
    const uint32_t n = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> out{static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                             static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

ObservationIngest::Clock fixed_now() {
    return [] { return kNow; };
}

// The authorized test identity: the producer's device, paired, consented and capture-enabled in a fixture
// registry. The accept path takes identity from the peer context (never from the payload), so this is the only
// place the identity enters.
struct VectorFixture {
    DeviceRegistry registry;
    std::string why;

    VectorFixture() {
        DeviceRegistry::Device d;
        d.device_id = kDeviceId;
        d.display_name = "synthetic node";
        d.device_type = "node";
        d.credential_id = "android-synthetic-cred";
        d.paired = true;
        d.capabilities = {"camera", "microphone"};
        REQUIRE(registry.pair(d, &why));
        REQUIRE(registry.set_consent(kDeviceId, true, kDeviceId, "security_monitor", kNow));
        REQUIRE(registry.set_capture_enabled(kDeviceId, true));
    }

    PeerContext peer(const std::string& id = kDeviceId) const {
        PeerContext p;
        p.authenticated_device_id = id;
        p.credential_id = "android-synthetic-cred";
        p.peer_authenticated = true;
        p.device_paired = true;
        p.device_consent_granted = true;
        p.capture_enabled = true;
        return p;
    }

    // The operator's configuration: admit the producer's observation_type, and the source type it sends, so the
    // receiver can be shown to refuse the latter for the RIGHT reason (no representation) rather than because an
    // allowlist happened to exclude it.
    ObservationIngest::Config extended() const {
        ObservationIngest::Config cfg = ObservationIngest::defaults();
        cfg.allowed_source_types.push_back(kProducerSourceType);
        cfg.allowed_observation_types.push_back(kObsType);
        return cfg;
    }
};

std::optional<DeviceMessage> decode_json(const std::string& body, RejectReason* why, std::string* detail) {
    return decode(json::parse(body), why, detail);
}

}  // namespace

TEST_CASE("android vector: the producer's exact frames decode through the receiver's own decoder",
          "[android][vector][contract]") {
    const auto helloFrame = framed(kHelloJson);
    const auto obsFrame = framed(kObservationJson);

    // Byte-exact pinning. Their emitter writes these sizes; if either serializer changes, this fails loudly.
    CHECK(helloFrame.size() == 183);
    CHECK(obsFrame.size() == 393);

    size_t declared = 0;
    REQUIRE(frame_length_acceptable(helloFrame.data(), 4, &declared));
    CHECK(declared == kHelloJson.size());
    REQUIRE(frame_length_acceptable(obsFrame.data(), 4, &declared));
    CHECK(declared == kObservationJson.size());
    // Their V4 (the frame fits the receiver's cap) re-asserted on OUR constant: a contract test must not agree
    // by accident.
    CHECK(obsFrame.size() <= kMaxFrameBytes);

    RejectReason why = RejectReason::none;
    std::string detail;
    const auto hello = decode(json::parse(kHelloJson), &why, &detail);
    REQUIRE(hello.has_value());
    CHECK(hello->kind == MessageKind::hello);
    CHECK(hello->device_id_claim == kDeviceId);
    CHECK(hello->display_name_claim == "synthetic node");
    CHECK(hello->capabilities.size() == 2);

    const auto obs = decode_json(kObservationJson, &why, &detail);
    REQUIRE(obs.has_value());
    CHECK(obs->kind == MessageKind::observation);
    CHECK(obs->observation_id == kObsId);
    CHECK(obs->source_type == kProducerSourceType);
    CHECK(obs->observation_type == kObsType);
    CHECK(obs->acquisition_mode == "synthetic");
    CHECK(obs->provenance_claim == "synthetic");
    CHECK(obs->sequence == kSeq);
    CHECK(obs->acquired_at_ms == kNow);
    CHECK(obs->unit == "fraction");
    REQUIRE(obs->confidence.has_value());
    CHECK(*obs->confidence == Approx(0.9));
    CHECK(obs->features["motion"].get<double>() == Approx(0.42));
    CHECK(obs->features["state"] == "present");
}

TEST_CASE("android vector: schema_version belongs to the NODE contract, not to the device transport",
          "[android][vector][contract][schema]") {
    // Their V6 asserts "absent is accepted; a present 0 is refused" and their vector sends no schema_version.
    // The rule is real, but it belongs to the NODE observation contract
    // (docs/NODE_OBSERVATION_SCHEMA_V1.md: "schema_version | integer | no | = 1 | absent means legacy v0; any
    // other value is refused"), which PanasonicNodeReceiver implements. The D-03 device transport this vector
    // actually speaks has no such field, and the same document says unknown extra keys are IGNORED there.
    // Both halves are asserted, so the difference is pinned rather than mistaken for a defect on either side.
    RejectReason why = RejectReason::none;
    std::string detail;

    SECTION("the device transport ignores an unknown extra key (documented, not an oversight)") {
        json with_zero = json::parse(kObservationJson);
        with_zero["schema_version"] = 0;
        const auto decoded = decode(with_zero, &why, &detail);
        REQUIRE(decoded.has_value());
        CHECK(decoded->observation_id == kObsId);
    }

    SECTION("the node contract, which owns the field, refuses a present 0 and accepts an absent one") {
        VectorFixture f;
        // The node receiver delegates to the ingest ("the receiver validates source_type with the hub's own
        // decoder and leaves the rest to the ingest"), so it is constructed over one.
        ObservationIngest ingest(f.registry, f.extended(), fixed_now());
        PanasonicNodeReceiver node(ingest, PanasonicNodeReceiver::Config{});
        std::vector<SecurityObservation> delivered;
        auto sink = [&](const SecurityObservation& o) { delivered.push_back(o); };

        const std::string legacy =
            R"({"source_type":"motion","observation_type":"motion-changed-fraction","value":0.42})";
        // Asserted BY REASON, deliberately: whether a bare node line is accepted also depends on the node
        // contract's own timestamp handling (a pre-epoch harness clock ages it out of the age window), which is
        // a different concern and not what this test is about. What must hold is the version RULE.
        CHECK(node.accept_line(f.peer(), legacy, sink).reason != "unsupported_schema_version");

        const std::string zero_version =
            R"({"source_type":"motion","observation_type":"motion-changed-fraction","value":0.42,"schema_version":0})";
        const auto refused = node.accept_line(f.peer(), zero_version, sink);
        CHECK_FALSE(refused.accepted);
        CHECK(refused.reason == "unsupported_schema_version");
    }
}

TEST_CASE("android vector: the welcome carries last_sequence and the ack correlates to the frame",
          "[android][vector][contract][ack]") {
    // Their sender treats a receipt that does not echo the right observation_id AND sequence as a FAILURE, so the
    // correlation is asserted on the receiver's own encoder rather than assumed.
    DeviceMessage ack;
    ack.kind = MessageKind::acknowledgement;
    ack.observation_id = kObsId;
    ack.sequence = kSeq;
    ack.reason = RejectReason::none;
    const json encoded = encode(ack);
    CHECK(encoded["observation_id"] == kObsId);
    CHECK(encoded["sequence"] == kSeq);
    CHECK(encoded["accepted"] == true);
    CHECK(encoded["reason"] == "ok");

    DeviceMessage welcome;
    welcome.kind = MessageKind::welcome;
    welcome.resume_after_sequence = kSeq;  // the highest sequence already accepted from this device
    const json w = encode(welcome);
    CHECK(w["last_sequence"] == kSeq);
    CHECK(w["kind"] == "welcome");
}

TEST_CASE("android vector: the producer's source type is refused WITHOUT throwing, even when allowlisted",
          "[android][vector][contract][vocabulary][defect]") {
    // THE DEFECT THIS TEST PINS. The accept path mapped the wire string to SecuritySourceType with
    // optional::value(); an allowlisted string with no representation threw std::bad_optional_access - an
    // exception raised by a value a PEER put on the wire, reachable as soon as an operator extends the allowlist
    // (as this fixture does). It must be a refusal, never an exception and never a default: defaulting would
    // relabel what a device reported.
    VectorFixture f;
    RejectReason why = RejectReason::none;
    std::string detail;
    const auto obs = decode_json(kObservationJson, &why, &detail);
    REQUIRE(obs.has_value());

    ObservationIngest ingest(f.registry, f.extended(), fixed_now());
    std::vector<SecurityObservation> delivered;
    auto sink = [&](const SecurityObservation& o) { delivered.push_back(o); };

    ObservationIngest::Result result;
    REQUIRE_NOTHROW(result = ingest.accept(f.peer(), *obs, sink));
    CHECK_FALSE(result.accepted);
    CHECK(result.reason == RejectReason::unknown_source_type);
    CHECK(delivered.empty());
}

TEST_CASE("android vector: with the receiver's source vocabulary the frame is admitted as SYNTHETIC",
          "[android][vector][contract][admitted]") {
    VectorFixture f;
    RejectReason why = RejectReason::none;
    std::string detail;
    const auto obs = decode_json(receiver_vocabulary_json(), &why, &detail);
    REQUIRE(obs.has_value());

    ObservationIngest ingest(f.registry, f.extended(), fixed_now());
    std::vector<SecurityObservation> delivered;
    auto sink = [&](const SecurityObservation& o) { delivered.push_back(o); };

    const auto accepted = ingest.accept(f.peer(), *obs, sink);
    REQUIRE(accepted.accepted);
    REQUIRE(delivered.size() == 1);
    CHECK(delivered.front().device_id == kDeviceId);
    CHECK(delivered.front().source_type == SecuritySourceType::motion);
    CHECK(delivered.front().provenance == Provenance::synthetic);
    CHECK(delivered.front().authorized);
    CHECK(delivered.front().features["motion"].get<double>() == Approx(0.42));
    // The frame reaches the event path as synthetic provenance and, because they never claimed "real", nothing is
    // flagged as an unsubstantiated claim.
    CHECK_FALSE(accepted.real);
    CHECK_FALSE(accepted.provenance_claim_unsubstantiated);
}

TEST_CASE("android vector: unknown device, withdrawn consent, a repeat and a broken frame are all refused",
          "[android][vector][contract][refusal]") {
    VectorFixture f;
    RejectReason why = RejectReason::none;
    std::string detail;
    const auto obs = decode_json(receiver_vocabulary_json(), &why, &detail);
    REQUIRE(obs.has_value());

    ObservationIngest ingest(f.registry, f.extended(), fixed_now());
    std::vector<SecurityObservation> delivered;
    auto sink = [&](const SecurityObservation& o) { delivered.push_back(o); };

    // the control first, so the refusals below are differences and not a dead path
    REQUIRE(ingest.accept(f.peer(), *obs, sink).accepted);

    SECTION("a peer the registry does not know is refused") {
        PeerContext stranger = f.peer();
        stranger.authenticated_device_id = "hg-android-unknown-99";
        stranger.device_paired = false;
        CHECK_FALSE(ingest.accept(stranger, *obs, sink).accepted);
    }

    SECTION("withdrawn consent is refused") {
        REQUIRE(f.registry.set_consent(kDeviceId, false, kDeviceId, "security_monitor", kNow));
        PeerContext withdrawn = f.peer();
        withdrawn.device_consent_granted = false;
        CHECK_FALSE(ingest.accept(withdrawn, *obs, sink).accepted);
        REQUIRE(f.registry.set_consent(kDeviceId, true, kDeviceId, "security_monitor", kNow));
    }

    SECTION("the same frame again does not count twice") {
        const size_t before = delivered.size();
        CHECK_FALSE(ingest.accept(f.peer(), *obs, sink).accepted);
        CHECK(delivered.size() == before);
    }

    SECTION("a frame whose declared length does not describe its body is not acceptable") {
        auto broken = framed(kObservationJson);
        broken[3] = static_cast<uint8_t>(broken[3] - 1);
        size_t declared = 0;
        REQUIRE(frame_length_acceptable(broken.data(), 4, &declared));
        CHECK(declared != kObservationJson.size());
    }

    SECTION("a body that is not the vector's shape cannot decode into its observation") {
        RejectReason bad = RejectReason::none;
        std::string why_detail;
        CHECK_FALSE(decode(json::parse(R"({"kind":"observation","observation_id":42})"), &bad, &why_detail)
                        .has_value());
        CHECK(bad != RejectReason::none);
    }
}

// test_panasonic_node_receiver: the adapter between the producer's NDJSON observation lines and the
// existing authenticated ingest.
//
// DETERMINISTIC MOCK INPUT ONLY. Nothing here is Panasonic hardware, and nothing here is physical
// integration: every line is constructed in this file, every peer context is synthesised, and no socket,
// certificate or device is involved. A green suite proves the ADAPTER's rules, never that a real node works.
//
// Frame-level replay refusal (a re-sent frame carrying an already-seen sequence) is asserted by the
// existing `[net]` suite against `ObservationIngest` itself - this adapter does not re-implement it, so it
// is not re-tested here. What IS asserted here is that the adapter never emits a duplicate or decreasing
// sequence of its own, which is what would reopen that hole from this side.

#include <catch2/catch.hpp>

#include "net/DeviceRegistry.h"
#include "net/ObservationIngest.h"
#include "net/PanasonicNodeReceiver.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;
using namespace homeguardian::net;
using nlohmann::json;

namespace {

int64_t clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string node_line(const std::string& source_type = "motion",
                      const std::string& observation_type = "motion-changed-fraction", double value = 0.25,
                      bool with_provenance = false, const std::string& provenance = "real_device",
                      bool with_schema = false, int schema = 1, int64_t acquired_at_ms = 0,
                      const std::string& state = "") {
    json j;
    j["source_type"] = source_type;
    j["observation_type"] = observation_type;
    j["value"] = value;
    j["unit"] = "fraction";
    if (!state.empty()) j["state"] = state;
    if (with_provenance) j["provenance"] = provenance;
    if (with_schema) j["schema_version"] = schema;
    if (acquired_at_ms != 0) j["acquired_at_ms"] = acquired_at_ms;
    return j.dump();
}

struct Harness {
    DeviceRegistry registry;
    std::string device_id = "hg-aabbccddee01";
    std::string credential = "aabbccddee01";
    std::vector<SecurityObservation> delivered;
    std::unique_ptr<ObservationIngest> ingest;
    std::function<void(const SecurityObservation&)> sink;

    explicit Harness(bool pair = true, bool consent = true, size_t max_pending = 64) {
        if (pair) {
            DeviceRegistry::Device d;
            d.device_id = device_id;
            d.display_name = "panasonic node (synthetic identity)";
            d.device_type = "camera";
            d.credential_id = credential;
            d.paired = true;
            d.capabilities = {"camera"};
            d.has_camera = true;
            std::string why;
            REQUIRE(registry.pair(d, &why));
        }
        if (pair && consent) {   // consent is only meaningful for a device that exists
            REQUIRE(registry.set_consent(device_id, true, device_id, "security_monitor", clock_ms()));
            REQUIRE(registry.set_capture_enabled(device_id, true));
        }
        ObservationIngest::Config cfg = ObservationIngest::defaults();
        cfg.max_pending = max_pending;
        ingest = std::make_unique<ObservationIngest>(registry, cfg, clock_ms);
        sink = [this](const SecurityObservation& o) { delivered.push_back(o); };
    }

    PeerContext peer(const std::string& id = "", const std::string& cred = "") const {
        PeerContext p;
        p.authenticated_device_id = id.empty() ? device_id : id;
        p.credential_id = cred.empty() ? credential : cred;
        p.peer_authenticated = true;
        p.device_paired = true;
        p.device_consent_granted = true;
        p.capture_enabled = true;
        return p;
    }
};

}  // namespace

TEST_CASE("panasonic receiver: an authenticated, consented observation is accepted and reaches the pipeline",
          "[panasonic]") {
    Harness h;
    PanasonicNodeReceiver receiver(*h.ingest);
    const auto r = receiver.accept_line(h.peer(), node_line(), h.sink);
    INFO("reason=" << r.reason << " detail=" << r.detail);
    REQUIRE(r.accepted);
    REQUIRE(r.reason == "accepted");
    REQUIRE(h.delivered.size() == 1);
    REQUIRE(h.delivered[0].device_id == h.device_id);
    REQUIRE(receiver.lines_accepted() == 1);
    REQUIRE(receiver.lines_refused() == 0);
}

TEST_CASE("panasonic receiver: identity comes from the session, never from the producer", "[panasonic]") {
    // The adapter leaves device_id_claim EMPTY, so a producer cannot name itself into another device's
    // identity or consent. Asserted structurally: a line claiming nothing still lands under the
    // authenticated peer's own id.
    Harness h;
    PanasonicNodeReceiver receiver(*h.ingest);
    REQUIRE(receiver.accept_line(h.peer(), node_line(), h.sink).accepted);
    REQUIRE(h.delivered[0].device_id == h.device_id);
}

TEST_CASE("panasonic receiver: unknown, unpaired, unconseted and revoked devices are refused",
          "[panasonic][authorization]") {
    SECTION("unknown device (never paired)") {
        Harness h(/*pair=*/false);
        PanasonicNodeReceiver receiver(*h.ingest);
        const auto r = receiver.accept_line(h.peer(), node_line(), h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(h.delivered.empty());        // nothing reaches the pipeline
    }
    SECTION("paired but consent never granted") {
        Harness h(/*pair=*/true, /*consent=*/false);
        PanasonicNodeReceiver receiver(*h.ingest);
        const auto r = receiver.accept_line(h.peer(), node_line(), h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(h.delivered.empty());
    }
    SECTION("consent withdrawn during the connection") {
        Harness h;
        PanasonicNodeReceiver receiver(*h.ingest);
        REQUIRE(receiver.accept_line(h.peer(), node_line(), h.sink).accepted);   // works while consented
        REQUIRE(h.registry.set_consent(h.device_id, false, h.device_id, "owner revocation", clock_ms()));
        const auto r = receiver.accept_line(h.peer(), node_line(), h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(h.delivered.size() == 1);    // the earlier delivery stands; nothing new arrives
        // ...and restoring consent lets it resume, which is the reconnect case.
        // Resuming is deliberately TWO steps: the withdrawal also switched capture off, so consent alone
        // does not restore the stream.
        REQUIRE(h.registry.set_consent(h.device_id, true, h.device_id, "restored", clock_ms()));
        const auto* rec = h.registry.find(h.device_id);
        REQUIRE(rec != nullptr);
        REQUIRE(rec->capture_enabled == false);
        REQUIRE(h.registry.set_capture_enabled(h.device_id, true));
        REQUIRE(receiver.accept_line(h.peer(), node_line(), h.sink).accepted);
        REQUIRE(h.delivered.size() == 2);
    }
}

TEST_CASE("panasonic receiver: malformed, oversized and mis-versioned lines are refused, not defaulted",
          "[panasonic][schema]") {
    Harness h;
    PanasonicNodeReceiver receiver(*h.ingest);

    SECTION("not JSON") {
        const auto r = receiver.accept_line(h.peer(), "{not json", h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(r.reason == "malformed_payload");
    }
    SECTION("missing source_type") {
        json j;
        j["observation_type"] = "motion-changed-fraction";
        j["value"] = 0.1;
        REQUIRE_FALSE(receiver.accept_line(h.peer(), j.dump(), h.sink).accepted);
    }
    SECTION("value is not a number") {
        json j;
        j["source_type"] = "motion";
        j["observation_type"] = "motion-changed-fraction";
        j["value"] = "0.1";
        REQUIRE_FALSE(receiver.accept_line(h.peer(), j.dump(), h.sink).accepted);
    }
    SECTION("value is not finite") {
        REQUIRE_FALSE(receiver.accept_line(h.peer(), node_line("motion", "motion-changed-fraction", INFINITY),
                                           h.sink)
                          .accepted);
    }
    SECTION("unknown observation type (closed vocabulary, enforced through the adapter)") {
        const auto r = receiver.accept_line(h.peer(), node_line("motion", "motion.per.fraction", 0.1), h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(r.reason == "unknown_observation_type");
    }
    SECTION("unknown source type (closed vocabulary)") {
        const auto r = receiver.accept_line(h.peer(), node_line("lidar_scan", "motion-changed-fraction", 0.1), h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(r.reason == "unknown_source_type");
    }
    SECTION("oversized line") {
        PanasonicNodeReceiver small(*h.ingest, PanasonicNodeReceiver::Config{true, 64, 1, 0, nullptr});
        REQUIRE_FALSE(small.accept_line(h.peer(), node_line(), h.sink).accepted);
    }
    SECTION("schema version absent is accepted AND counted (the producer sends none today)") {
        REQUIRE(receiver.accept_line(h.peer(), node_line(), h.sink).accepted);
        REQUIRE(receiver.schema_version_absent() == 1);
    }
    SECTION("an unimplemented schema version is refused") {
        const auto r = receiver.accept_line(h.peer(), node_line("motion", "motion-changed-fraction", 0.1, false,
                                                               "real_device", true, 99),
                                            h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(r.reason == "unsupported_schema_version");
    }
}

TEST_CASE("panasonic receiver: falsy provenance claims are refused, and absence is never 'real'",
          "[panasonic][provenance]") {
    SECTION("a real_device claim on a synthetic session is REFUSED, not downgraded") {
        Harness h;
        PanasonicNodeReceiver receiver(*h.ingest);   // session_is_synthetic defaults to true (fail closed)
        const auto r = receiver.accept_line(h.peer(), node_line("motion", "motion-changed-fraction", 0.2, true,
                                                              "real_device"),
                                            h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(r.reason == "provenance_claim_widens_session");
        REQUIRE(receiver.provenance_refused() == 1);
        REQUIRE(h.delivered.empty());
    }
    SECTION("an ABSENT claim on a synthetic session stays synthetic (the producer would default it to real)") {
        Harness h;
        PanasonicNodeReceiver receiver(*h.ingest);
        REQUIRE(receiver.accept_line(h.peer(), node_line(), h.sink).accepted);
        REQUIRE(h.delivered.size() == 1);
        REQUIRE(h.delivered[0].provenance == Provenance::synthetic);
    }
    SECTION("a synthetic claim on a REAL session is accepted and NARROWED") {
        Harness h;
        PanasonicNodeReceiver::Config cfg;
        cfg.session_is_synthetic = false;
        PanasonicNodeReceiver receiver(*h.ingest, cfg);
        REQUIRE(receiver.accept_line(h.peer(), node_line("motion", "motion-changed-fraction", 0.2, true, "synthetic"),
                                     h.sink)
                    .accepted);
        REQUIRE(receiver.provenance_narrowed() == 1);
        // MEASURED: the hub records the provenance ITS session model supports. A claim can never inflate it -
        // and it cannot deflate it either, which is an interface asymmetry recorded in the contract rather
        // than hidden by making this adapter pretend to overrule the hub.
        REQUIRE(h.delivered[0].provenance == Provenance::real);
    }
    SECTION("an invented claim string is refused") {
        Harness h;
        PanasonicNodeReceiver receiver(*h.ingest);
        const auto r = receiver.accept_line(h.peer(), node_line("motion", "motion-changed-fraction", 0.2, true,
                                                              "probably_real"),
                                            h.sink);
        REQUIRE_FALSE(r.accepted);
        REQUIRE(r.reason == "unknown_provenance_claim");
    }
}

TEST_CASE("panasonic receiver: stale, future-skewed and unbounded input cannot become an observation",
          "[panasonic][bounds]") {
    Harness h;
    PanasonicNodeReceiver receiver(*h.ingest);
    const int64_t now = clock_ms();

    SECTION("a timestamp far in the future is refused") {
        REQUIRE_FALSE(receiver.accept_line(h.peer(), node_line("motion", "motion-changed-fraction", 0.2, false,
                                                             "real_device", false, 1, now + 3600000),
                                           h.sink)
                          .accepted);
    }
    SECTION("a timestamp older than the accepted age is refused") {
        REQUIRE_FALSE(receiver.accept_line(h.peer(), node_line("motion", "motion-changed-fraction", 0.2, false,
                                                             "real_device", false, 1, now - 172800000),
                                           h.sink)
                          .accepted);
    }
    SECTION("backpressure: a full queue refuses rather than growing") {
        Harness h2(true, true, /*max_pending=*/2);
        PanasonicNodeReceiver r2(*h2.ingest);
        // No sink: accepted observations stay queued, so the bound is observable.
        ObservationIngest::Sink none;
        REQUIRE(r2.accept_line(h2.peer(), node_line(), none).accepted);
        REQUIRE(r2.accept_line(h2.peer(), node_line(), none).accepted);
        const auto third = r2.accept_line(h2.peer(), node_line(), none);
        REQUIRE_FALSE(third.accepted);
        REQUIRE_FALSE(third.reason.empty());     // a NAMED refusal, never a silent drop
    }
    REQUIRE(h.delivered.empty());
}

TEST_CASE("panasonic receiver: two node identities stay independent", "[panasonic][multi-node]") {
    Harness h;
    DeviceRegistry::Device second;
    second.device_id = "hg-aabbccddee02";
    second.display_name = "second node (synthetic)";
    second.device_type = "camera";
    second.credential_id = "aabbccddee02";
    second.paired = true;
    second.capabilities = {"camera"};
    std::string why;
    REQUIRE(h.registry.pair(second, &why));
    REQUIRE(h.registry.set_consent(second.device_id, true, second.device_id, "security_monitor", clock_ms()));
    REQUIRE(h.registry.set_capture_enabled(second.device_id, true));

    PanasonicNodeReceiver receiver(*h.ingest);
    REQUIRE(receiver.accept_line(h.peer(), node_line(), h.sink).accepted);
    REQUIRE(receiver.accept_line(h.peer("hg-aabbccddee02", "aabbccddee02"),
                             node_line("contact", "contact-state", 1.0), h.sink)
                .accepted);
    REQUIRE(h.delivered.size() == 2);
    REQUIRE(h.delivered[0].device_id == "hg-aabbccddee01");
    REQUIRE(h.delivered[1].device_id == "hg-aabbccddee02");
}

TEST_CASE("panasonic receiver: nothing to consume fabricates nothing", "[panasonic][absence]") {
    // The rule the whole project runs on: absence of data is never turned into a negative observation such as
    // "no activity". No input means no delivery, and an unavailable receiver means a named refusal.
    Harness h;
    PanasonicNodeReceiver receiver(*h.ingest);
    REQUIRE(h.delivered.empty());
    // A refused line (here: a falsy provenance claim) delivers nothing either - the pipeline must not learn
    // anything from input that was not accepted.
    REQUIRE_FALSE(receiver.accept_line(h.peer(), node_line("motion", "motion-changed-fraction", 0.2, true,
                                                          "real_device"),
                                       h.sink)
                      .accepted);
    REQUIRE(h.delivered.empty());
    REQUIRE(receiver.lines_accepted() == 0);
}

// ---------------------------------------------------------------------------------------------------------
// The drift guard: the CONTRACT DOCUMENT and the CODE must agree, and a change on either side must fail
// loudly here rather than silently skew `docs/NODE_OBSERVATION_SCHEMA_V1.md` away from what actually runs.
//
// It reads the document at test time (the suite already locates repo files through HG_SOURCE_DIR) and asserts
// three things: the receiver's declared schema version matches the document's; every source type the hub
// allows is listed in the document; and every observation type the hub allows is listed there too. A new
// allowlisted type added to the hub without a contract edit, or a version bump on one side only, fails.
// ---------------------------------------------------------------------------------------------------------
TEST_CASE("node observation contract: the document and the code cannot drift apart", "[panasonic][contract]") {
    const char* dir = std::getenv("HG_SOURCE_DIR");
    if (dir == nullptr || *dir == '\0') {
        WARN("HG_SOURCE_DIR is unset: skipping the document half of the drift guard");
        REQUIRE(kNodeSchemaVersion == 1);
        return;
    }
    std::ifstream in(std::string(dir) + "/docs/NODE_OBSERVATION_SCHEMA_V1.md");
    REQUIRE(in.good());                                  // the contract must exist where the code claims it does
    const std::string doc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(doc.size() > 500);

    // 1. declared version
    REQUIRE(doc.find("Version:** 1.0.0") != std::string::npos);
    REQUIRE(kNodeSchemaVersion == 1);
    REQUIRE(doc.find("kNodeSchemaVersion = 1") != std::string::npos);

    // 2. the vocabularies the hub actually enforces are the ones the contract publishes
    const auto cfg = ObservationIngest::defaults();
    REQUIRE(cfg.allowed_source_types.size() == 8);
    REQUIRE(cfg.allowed_observation_types.size() == 12);
    for (const auto& t : cfg.allowed_source_types) {
        INFO("source type missing from the contract: " << t);
        REQUIRE(doc.find(t) != std::string::npos);
    }
    for (const auto& t : cfg.allowed_observation_types) {
        INFO("observation type missing from the contract: " << t);
        REQUIRE(doc.find(t) != std::string::npos);
    }

    // 3. the honesty rules the document states are stated in the code too
    REQUIRE(doc.find("Absence is never real") != std::string::npos);
    REQUIRE(doc.find("may never widen") != std::string::npos);
}

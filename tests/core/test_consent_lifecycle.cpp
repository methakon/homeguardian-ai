// H-03 consent-lifecycle regression tests.
//
// These exist because of a defect reproduced on the physical phone: the operator pressed
// "Grant consent" more than once, the repository appended a second granted record, and the gate -
// correctly - refused the resulting ambiguity ("mic: initialize denied by gate",
// DenyReason::ambiguous). The fix is a repository that maintains at most one active grant per
// (subject, purpose). These tests pin BOTH halves:
//   * the repository can never produce more than one active grant, however often grant() is
//     pressed, and a withdrawal is final; and
//   * the GATE still refuses ambiguity when it is genuinely handed conflicting grants - so nobody
//     can "fix" this class of bug by weakening the fail-closed rule.

#include "catch2/catch.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/ConsentGate.h"
#include "core/ConsentRecord.h"
#include "persistence/SingleActiveGrantConsentRepo.h"

using namespace homeguardian;

namespace {

constexpr const char* kSubject = "node";
constexpr const char* kCamera = "camera_acquire";
constexpr const char* kMic = "mic_acquire";

AcquisitionRequest cam() { return {kSubject, kCamera, "camera"}; }
AcquisitionRequest mic() { return {kSubject, kMic, "audio"}; }

} // namespace

TEST_CASE("consent lifecycle: a single grant authorizes both camera and microphone",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, /*media_capture_enabled=*/true);

    REQUIRE(gate.check(cam()).reason == DenyReason::no_consent_record);
    REQUIRE(gate.check(mic()).reason == DenyReason::no_consent_record);

    repo.grant(kSubject, kCamera, {"camera"});
    repo.grant(kSubject, kMic, {"audio"});

    const auto c = gate.check(cam());
    const auto m = gate.check(mic());
    REQUIRE(c.authorized);
    REQUIRE(c.reason == DenyReason::none);
    REQUIRE(m.authorized);
    REQUIRE(repo.active_grant_count(kSubject, kCamera) == 1);
    REQUIRE(repo.active_grant_count(kSubject, kMic) == 1);
}

TEST_CASE("consent lifecycle: repeated grant presses cannot create ambiguity (H-03 regression)",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    // This is exactly what the operator did on the phone: pressed Grant consent several times.
    for (int i = 0; i < 5; ++i) {
        repo.grant(kSubject, kCamera, {"camera"});
        repo.grant(kSubject, kMic, {"audio"});
    }

    REQUIRE(repo.active_grant_count(kSubject, kCamera) == 1);
    REQUIRE(repo.active_grant_count(kSubject, kMic) == 1);

    const auto c = gate.check(cam());
    const auto m = gate.check(mic());
    REQUIRE(c.authorized);
    REQUIRE(m.authorized);
    REQUIRE(m.reason != DenyReason::ambiguous);
}

TEST_CASE("consent lifecycle: a hundred presses still leave exactly one active grant",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    for (int i = 0; i < 100; ++i) repo.grant(kSubject, kMic, {"audio"});

    REQUIRE(repo.active_grant_count(kSubject, kMic) == 1);
    REQUIRE(gate.check(mic()).authorized);
}

TEST_CASE("consent lifecycle: grant() is idempotent and reports what it superseded",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;

    REQUIRE(repo.grant(kSubject, kCamera, {"camera"}) == 0);  // first grant supersedes nothing
    REQUIRE(repo.grant(kSubject, kCamera, {"camera"}) == 1);  // repeat supersedes the previous one
    REQUIRE(repo.grant(kSubject, kCamera, {"camera"}) == 1);
    REQUIRE(repo.count() == 1);                               // no record accumulation
}

TEST_CASE("consent lifecycle: the gate STILL refuses two genuine active grants",
          "[consent_lifecycle]") {
    // Deliberately bypass the lifecycle with the raw store to hand the gate a conflicting set.
    // If this ever stops reporting `ambiguous`, the fail-closed rule has been weakened.
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    repo.save(ConsentRecord::create_granted("raw-1", kSubject, kMic, {"audio"}, "1.0.0", "op"));
    repo.save(ConsentRecord::create_granted("raw-2", kSubject, kMic, {"audio"}, "1.0.0", "op"));

    REQUIRE(repo.active_grant_count(kSubject, kMic) == 2);
    const auto d = gate.check(mic());
    REQUIRE_FALSE(d.authorized);
    REQUIRE(d.reason == DenyReason::ambiguous);
    REQUIRE(ConsentGate::reason_to_string(d.reason) == "ambiguous");
}

TEST_CASE("consent lifecycle: a later withdrawal removes every granted record and denies",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    for (int i = 0; i < 3; ++i) repo.grant(kSubject, kMic, {"audio"});
    REQUIRE(gate.check(mic()).authorized);

    REQUIRE(repo.withdraw(kSubject, kMic) == 1);  // removed the one active grant

    const auto d = gate.check(mic());
    REQUIRE_FALSE(d.authorized);
    REQUIRE(d.reason == DenyReason::decision_not_granted);
    REQUIRE(repo.active_grant_count(kSubject, kMic) == 0);

    // No granted record survives, so nothing can be resurrected later.
    for (const auto& r : repo.all()) {
        if (r.get_subject_scope() == kSubject && r.get_purpose() == kMic) {
            REQUIRE(r.get_decision() == ConsentDecision::withdrawn);
        }
    }
}

TEST_CASE("consent lifecycle: withdrawal is final even if the whole history is replayed",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    repo.grant(kSubject, kMic, {"audio"});
    repo.withdraw(kSubject, kMic);

    // Feed the complete surviving history into a naive appending store: the latest decision is the
    // withdrawal, so the gate must still deny.
    SingleActiveGrantConsentRepo replayed;
    for (const auto& r : repo.all()) replayed.save(r);

    ConsentGate gate(replayed, true);
    const auto d = gate.check(mic());
    REQUIRE_FALSE(d.authorized);
    REQUIRE(d.reason == DenyReason::decision_not_granted);
}

TEST_CASE("consent lifecycle: repeated withdrawals stay denied", "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    repo.grant(kSubject, kMic, {"audio"});
    repo.withdraw(kSubject, kMic);
    repo.withdraw(kSubject, kMic);

    REQUIRE_FALSE(gate.check(mic()).authorized);
    REQUIRE(repo.active_grant_count(kSubject, kMic) == 0);
}

TEST_CASE("consent lifecycle: consent can be granted again after a withdrawal",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    repo.grant(kSubject, kMic, {"audio"});
    repo.withdraw(kSubject, kMic);
    REQUIRE_FALSE(gate.check(mic()).authorized);

    repo.grant(kSubject, kMic, {"audio"});
    const auto d = gate.check(mic());
    REQUIRE(d.authorized);
    REQUIRE(repo.active_grant_count(kSubject, kMic) == 1);
}

TEST_CASE("consent lifecycle: camera and microphone decisions are independent",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    repo.grant(kSubject, kCamera, {"camera"});

    REQUIRE(gate.check(cam()).authorized);
    REQUIRE_FALSE(gate.check(mic()).authorized);
    REQUIRE(gate.check(mic()).reason == DenyReason::no_consent_record);
}

TEST_CASE("consent lifecycle: a category the grant does not cover is still refused",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    repo.grant(kSubject, kCamera, {"camera"});   // grants camera, not audio
    const auto d = gate.check({kSubject, kCamera, "audio"});
    REQUIRE_FALSE(d.authorized);
    REQUIRE(d.reason == DenyReason::category_missing);
}

TEST_CASE("consent lifecycle: the global capture switch still overrides any grant",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, /*media_capture_enabled=*/false);

    repo.grant(kSubject, kMic, {"audio"});
    const auto d = gate.check(mic());
    REQUIRE_FALSE(d.authorized);
    REQUIRE(d.reason == DenyReason::capture_disabled);
}

TEST_CASE("consent lifecycle: concurrent presses keep the single-active-grant invariant",
          "[consent_lifecycle]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);

    std::atomic<int> done{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < 50; ++i) repo.grant(kSubject, kMic, {"audio"});
            ++done;
        });
    }
    for (auto& w : workers) w.join();

    REQUIRE(done.load() == 4);
    REQUIRE(repo.active_grant_count(kSubject, kMic) == 1);
    REQUIRE(gate.check(mic()).authorized);
}

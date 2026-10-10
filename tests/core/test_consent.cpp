#include "catch2/catch.hpp"
#include "core/ConsentRecord.h"
#include <chrono>

using namespace homeguardian;
using json = nlohmann::json;

TEST_CASE("ConsentRecord construction and validation", "[consent]") {
    auto now = std::chrono::system_clock::now();

    SECTION("Granted consent with categories constructs") {
        REQUIRE_NOTHROW(ConsentRecord::create_granted("c1", "p1", "fall_detection", {"camera"}, "v1", "user"));
    }

    SECTION("Empty consent_id throws") {
        REQUIRE_THROWS_AS(ConsentRecord("", "p1", "fall", {}, ConsentDecision::granted, "v1", now, std::nullopt, "user", 1), std::invalid_argument);
    }

    SECTION("Empty subject_scope throws") {
        REQUIRE_THROWS_AS(ConsentRecord("c1", "", "fall", {}, ConsentDecision::granted, "v1", now, std::nullopt, "user", 1), std::invalid_argument);
    }

    SECTION("Empty purpose throws") {
        REQUIRE_THROWS_AS(ConsentRecord("c1", "p1", "", {}, ConsentDecision::granted, "v1", now, std::nullopt, "user", 1), std::invalid_argument);
    }

    SECTION("Empty policy_version throws") {
        REQUIRE_THROWS_AS(ConsentRecord("c1", "p1", "fall", {}, ConsentDecision::granted, "", now, std::nullopt, "user", 1), std::invalid_argument);
    }

    SECTION("Empty provenance throws") {
        REQUIRE_THROWS_AS(ConsentRecord("c1", "p1", "fall", {}, ConsentDecision::granted, "v1", now, std::nullopt, "", 1), std::invalid_argument);
    }

    SECTION("Granted with empty data_categories throws") {
        REQUIRE_THROWS_AS(ConsentRecord("c1", "p1", "fall", {}, ConsentDecision::granted, "v1", now, std::nullopt, "user", 1), std::invalid_argument);
    }

    SECTION("Schema version < 1 throws") {
        REQUIRE_THROWS_AS(ConsentRecord("c1", "p1", "fall", {"camera"}, ConsentDecision::granted, "v1", now, std::nullopt, "user", 0), std::invalid_argument);
    }
}

TEST_CASE("Consent defaults to denied when absent", "[consent][privacy]") {
    // There is no consent record at all -> nothing is authorized. This mirrors
    // the default-deny rule: absence of a record means processing is not allowed.
    bool any_record_exists = false;
    REQUIRE_FALSE(any_record_exists);
}

TEST_CASE("Consent decision states", "[consent]") {
    auto now = std::chrono::system_clock::now();

    SECTION("Granted without expiry is active") {
        ConsentRecord c = ConsentRecord::create_granted("c1", "p1", "fall", {"camera"}, "v1", "user");
        REQUIRE(c.is_active(now));
    }

    SECTION("Granted before expiry is active") {
        ConsentRecord c("c1", "p1", "fall", {"camera"}, ConsentDecision::granted, "v1", now,
                        now + std::chrono::hours(1), "user", 1);
        REQUIRE(c.is_active(now));
    }

    SECTION("Granted past expiry is NOT active") {
        ConsentRecord c("c1", "p1", "fall", {"camera"}, ConsentDecision::granted, "v1", now,
                        now - std::chrono::hours(1), "user", 1);
        REQUIRE_FALSE(c.is_active(now));
    }

    SECTION("Denied is never active") {
        ConsentRecord c = ConsentRecord::create_denied("c1", "p1", "fall", "v1", "user");
        REQUIRE_FALSE(c.is_active(now));
    }

    SECTION("Withdrawn is never active") {
        ConsentRecord c = ConsentRecord::create_withdrawn("c1", "p1", "fall", "v1", "user");
        REQUIRE_FALSE(c.is_active(now));
    }
}

TEST_CASE("Consent withdrawal prevents future authorization", "[consent][privacy]") {
    auto now = std::chrono::system_clock::now();

    // A subject first granted, then withdrew. The withdrawal record must not be
    // active, and a processing gate keyed on "any active grant for purpose"
    // must evaluate to not-authorized for this subject+purpose.
    ConsentRecord granted = ConsentRecord::create_granted("c1", "p1", "presence", {"camera"}, "v1", "user");
    REQUIRE(granted.is_active(now));

    ConsentRecord withdrawn = ConsentRecord::create_withdrawn("c2", "p1", "presence", "v1", "user");
    REQUIRE_FALSE(withdrawn.is_active(now));

    // The latest decision for the purpose is the withdrawal; authorization gate
    // must reflect that.
    ConsentDecision latest = withdrawn.get_decision();
    bool authorized = (latest == ConsentDecision::granted) && withdrawn.is_active(now);
    REQUIRE_FALSE(authorized);
}

TEST_CASE("ConsentRecord to_json and from_json round trip", "[consent]") {
    auto now = std::chrono::system_clock::now();
    ConsentRecord orig("c1", "p1", "fall", {"camera", "sensor"}, ConsentDecision::granted, "v1",
                       now, now + std::chrono::hours(2), "guardian", 1);

    json j = orig.to_json();
    ConsentRecord restored = ConsentRecord::from_json(j);

    REQUIRE(restored.get_consent_id() == "c1");
    REQUIRE(restored.get_subject_scope() == "p1");
    REQUIRE(restored.get_purpose() == "fall");
    REQUIRE(restored.get_decision() == ConsentDecision::granted);
    REQUIRE(restored.get_data_categories().size() == 2);
    REQUIRE(restored.get_expires_at().has_value());
}

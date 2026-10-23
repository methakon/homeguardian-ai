#include "catch2/catch.hpp"
#include "core/AlexaContract.h"
#include "core/Logger.h"
#include <chrono>
#include <string>
#include <variant>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// AlexaContract: versioned, authenticated, read-only integration contract.
// Deterministic, local, no network/AWS/credentials. Simulator-only.
// ---------------------------------------------------------------------------

namespace {

// A deterministic credential verifier driven by a table of known tokens.
// It NEVER trusts a client-supplied identity: a token is only accepted if it
// matches a known entry AND is not expired at `now`.
class FakeVerifier : public IAlexaCredentialVerifier {
public:
    struct Entry {
        std::string token;
        std::string principal;
        bool allow_status = false;
        bool allow_alert_summary = false;
        std::chrono::system_clock::time_point expires_at{};
        // Appended LAST on purpose: the fixtures below initialize Entry positionally, so a field
        // inserted earlier would silently rebind their values. Absent => false, which is the
        // default-deny that keeps device visibility off every pre-existing token.
        bool allow_devices = false;
    };

    void add(Entry e) { entries_.push_back(std::move(e)); }

    AlexaAuthContext verify(const std::string& credential,
                            std::chrono::system_clock::time_point now) const override {
        AlexaAuthContext ctx;  // defaults: authenticated=false
        for (const auto& e : entries_) {
            if (e.token == credential) {
                if (now < e.expires_at) {
                    ctx.authenticated = true;
                    ctx.principal = e.principal;
                    ctx.allow_status = e.allow_status;
                    ctx.allow_alert_summary = e.allow_alert_summary;
                    ctx.allow_devices = e.allow_devices;
                }
                return ctx;  // known token: authorized only if not expired
            }
        }
        return ctx;  // unknown token: fail closed
    }

private:
    std::vector<Entry> entries_;
};

// A deterministic status provider with a configurable snapshot.
class FakeProvider : public IAlexaStatusProvider {
public:
    AlexaStatusSnapshot snap;
    AlexaStatusSnapshot snapshot() const override { return snap; }
};

FakeVerifier make_verifier(std::chrono::system_clock::time_point base) {
    FakeVerifier v;
    auto far_future = base + std::chrono::hours(24 * 365);
    // Full-access token.
    v.add({"tok-full", "alexa-user", true, true, far_future});
    // Device-visibility token: permitted for the devices operation ONLY.
    v.add({"tok-devices", "portal-viewer", false, false, far_future, true});
    // Status-only token.
    v.add({"tok-status", "alexa-user", true, false, far_future});
    // No-permission token (authenticated but forbidden everywhere).
    v.add({"tok-noperm", "alexa-user", false, false, far_future});
    // Expired token.
    v.add({"tok-expired", "alexa-user", true, true, base - std::chrono::hours(1)});
    return v;
}

std::string valid_body(const std::string& op, const std::string& token) {
    return std::string("{\"version\":1,\"operation\":\"") + op +
           "\",\"credential\":\"" + token + "\"}";
}

// A request body that additionally asserts client-supplied identity/permission
// fields. These are UNKNOWN fields and must be rejected as malformed.
std::string spoofed_body(const std::string& op, const std::string& token) {
    return std::string("{\"version\":1,\"operation\":\"") + op +
           "\",\"credential\":\"" + token +
           "\",\"principal\":\"admin\",\"allow_status\":true,\"is_admin\":true}";
}

// Extract the error from a parse result (assumes parse failed).
AlexaError parse_error(const std::string& body) {
    auto r = AlexaContract::parse(body);
    REQUIRE(std::holds_alternative<AlexaError>(r));
    return std::get<AlexaError>(r);
}

// Parse a body that is expected to succeed, returning the request.
AlexaRequest parse_ok(const std::string& body) {
    auto r = AlexaContract::parse(body);
    REQUIRE(std::holds_alternative<AlexaRequest>(r));
    return std::get<AlexaRequest>(r);
}

}  // namespace

// --- 1. Valid authenticated read-only status request ---
TEST_CASE("Alexa: valid authenticated status request returns read-only state",
          "[alexa][contract]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    p.snap.state = "Running";
    p.snap.capture_enabled = false;

    auto parsed = AlexaContract::parse(valid_body("status", "tok-full"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE(resp.ok);
    REQUIRE(resp.error == AlexaError::none);
    REQUIRE(resp.state == "Running");
    REQUIRE_FALSE(resp.capture_enabled);
}

// --- 2. Valid authenticated alert-summary request ---
TEST_CASE("Alexa: valid authenticated alert_summary returns aggregate only",
          "[alexa][contract]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    p.snap.state = "Running";
    p.snap.alert_count = 2;
    p.snap.highest_severity = "warning";
    p.snap.alert_ids = {"a-1", "a-2"};

    auto parsed = AlexaContract::parse(valid_body("alert_summary", "tok-full"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE(resp.ok);
    REQUIRE(resp.alert_count == 2);
    REQUIRE(resp.highest_severity == "warning");
    REQUIRE(resp.alert_ids.size() == 2);
}

TEST_CASE("Alexa: alert_summary caps returned ids and handles empty result",
          "[alexa][contract]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;

    // Empty: safe response, no crash.
    p.snap.alert_count = 0;
    p.snap.highest_severity = "";
    auto r0 = AlexaContract::handle(parse_ok(valid_body("alert_summary", "tok-full")),
                                    v, p, now);
    REQUIRE(r0.ok);
    REQUIRE(r0.alert_count == 0);
    REQUIRE(r0.highest_severity.empty());
    REQUIRE(r0.alert_ids.empty());

    // Over-cap: ids are truncated to ALEXA_MAX_ALERTS_RETURNED.
    for (size_t i = 0; i < ALEXA_MAX_ALERTS_RETURNED + 3; ++i) {
        p.snap.alert_ids.push_back("id-" + std::to_string(i));
    }
    p.snap.alert_count = p.snap.alert_ids.size();
    auto rc = AlexaContract::handle(parse_ok(valid_body("alert_summary", "tok-full")),
                                    v, p, now);
    REQUIRE(rc.ok);
    REQUIRE(rc.alert_ids.size() == ALEXA_MAX_ALERTS_RETURNED);
}

// --- 3. Missing and invalid authentication ---
TEST_CASE("Alexa: missing credential is rejected at parse (malformed)",
          "[alexa][auth]") {
    Logger::initialize("error");
    REQUIRE(parse_error("{\"version\":1,\"operation\":\"status\"}") == AlexaError::malformed);
}

TEST_CASE("Alexa: invalid credential fails closed with unauthorized",
          "[alexa][auth]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    auto parsed = AlexaContract::parse(valid_body("status", "tok-bogus"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.error == AlexaError::unauthorized);
}

TEST_CASE("Alexa: expired credential fails closed with unauthorized",
          "[alexa][auth]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    auto parsed = AlexaContract::parse(valid_body("status", "tok-expired"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.error == AlexaError::unauthorized);
}

// --- 4. Unauthorized operation or identity ---
TEST_CASE("Alexa: authenticated but unauthorized operation is forbidden",
          "[alexa][auth]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    // tok-status may read status but NOT alert_summary.
    auto parsed = AlexaContract::parse(valid_body("alert_summary", "tok-status"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.error == AlexaError::forbidden);
}

TEST_CASE("Alexa: authenticated principal with no permissions is forbidden",
          "[alexa][auth]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    auto parsed = AlexaContract::parse(valid_body("status", "tok-noperm"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.error == AlexaError::forbidden);
}

// --- 5. Malformed and oversized requests ---
TEST_CASE("Alexa: malformed bodies are rejected",
          "[alexa][malformed]") {
    Logger::initialize("error");
    REQUIRE(parse_error("") == AlexaError::malformed);                       // empty
    REQUIRE(parse_error("not json") == AlexaError::malformed);               // not JSON
    REQUIRE(parse_error("[]") == AlexaError::malformed);                     // not object
    REQUIRE(parse_error("{\"version\":1}") == AlexaError::malformed);        // no op/cred
    REQUIRE(parse_error("{\"version\":\"1\",\"operation\":\"status\","
                        "\"credential\":\"t\"}") == AlexaError::malformed);  // version type
    REQUIRE(parse_error("{\"version\":1,\"operation\":\"enable_capture\","
                        "\"credential\":\"t\"}") == AlexaError::malformed);  // unknown op
    REQUIRE(parse_error("{\"version\":1,\"operation\":\"status\","
                        "\"credential\":\"t\",\"extra\":1}") == AlexaError::malformed);  // unknown field
    REQUIRE(parse_error("{\"version\":1,\"operation\":\"status\","
                        "\"credential\":\"\"}") == AlexaError::malformed);   // empty cred
}

TEST_CASE("Alexa: oversized request is rejected without parsing",
          "[alexa][malformed]") {
    Logger::initialize("error");
    std::string big(ALEXA_MAX_REQUEST_BYTES + 1, 'x');
    REQUIRE(parse_error(big) == AlexaError::malformed);
}

// --- 6. Unsupported contract version ---
TEST_CASE("Alexa: unsupported version is rejected distinctly",
          "[alexa][version]") {
    Logger::initialize("error");
    REQUIRE(parse_error("{\"version\":2,\"operation\":\"status\","
                        "\"credential\":\"t\"}") == AlexaError::unsupported_version);
    REQUIRE(parse_error("{\"version\":0,\"operation\":\"status\","
                        "\"credential\":\"t\"}") == AlexaError::unsupported_version);
}

// --- 7. Application unavailable or degraded ---
TEST_CASE("Alexa: application unavailable fails closed",
          "[alexa][availability]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = false;  // degraded
    auto parsed = AlexaContract::parse(valid_body("status", "tok-full"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.error == AlexaError::unavailable);
}

// --- 8. Safe response when no alerts exist (covered above) + error messages ---
TEST_CASE("Alexa: error messages are generic and leak no internals",
          "[alexa][errors]") {
    Logger::initialize("error");
    std::string m = AlexaContract::error_message(AlexaError::unauthorized);
    // No paths, no stack details, no field names, no "token"/"credential".
    REQUIRE(m.find("/") == std::string::npos);
    REQUIRE(m.find("token") == std::string::npos);
    REQUIRE(m.find("credential") == std::string::npos);
    REQUIRE(m.find("sqlite") == std::string::npos);
    REQUIRE_FALSE(m.empty());
}

TEST_CASE("Alexa: contract exposes no mutating operation",
          "[alexa][safety]") {
    // The only accepted operations are reads; anything else is malformed.
    Logger::initialize("error");
    REQUIRE(parse_error(valid_body("enable_capture", "tok-full")) == AlexaError::malformed);
    REQUIRE(parse_error(valid_body("grant_consent", "tok-full")) == AlexaError::malformed);
    REQUIRE(parse_error(valid_body("set_config", "tok-full")) == AlexaError::malformed);
    REQUIRE(parse_error(valid_body("activate_sensor", "tok-full")) == AlexaError::malformed);
}

// --- F22-31 security-review regression cases ---

TEST_CASE("Alexa: deeply nested JSON within the size bound is rejected",
          "[alexa][safety]") {
    // A nesting bomb under the 4KB request bound must not crash or hang; the
    // contract rejects it as malformed.
    Logger::initialize("error");
    std::string nested = "{\"version\":1,\"operation\":\"status\",\"credential\":\"t\",\"x\":";
    nested += std::string(1500, '[');
    nested += std::string(1500, ']');
    nested += "}";
    REQUIRE(nested.size() <= ALEXA_MAX_REQUEST_BYTES);
    REQUIRE(parse_error(nested) == AlexaError::malformed);
}

TEST_CASE("Alexa: non-object JSON top level is rejected",
          "[alexa][safety]") {
    Logger::initialize("error");
    REQUIRE(parse_error("[1,2,3]") == AlexaError::malformed);
    REQUIRE(parse_error("\"a string\"") == AlexaError::malformed);
    REQUIRE(parse_error("42") == AlexaError::malformed);
    REQUIRE(parse_error("null") == AlexaError::malformed);
}

TEST_CASE("Alexa: trailing data after a JSON object is rejected",
          "[alexa][safety]") {
    Logger::initialize("error");
    REQUIRE(parse_error(valid_body("status", "t") + " garbage") == AlexaError::malformed);
    REQUIRE(parse_error(valid_body("status", "t") + valid_body("status", "t")) ==
            AlexaError::malformed);
}

TEST_CASE("Alexa: wrong field types are rejected (malformed)",
          "[alexa][safety]") {
    Logger::initialize("error");
    // operation not a string.
    REQUIRE(parse_error("{\"version\":1,\"operation\":123,\"credential\":\"t\"}") ==
            AlexaError::malformed);
    // credential not a string.
    REQUIRE(parse_error("{\"version\":1,\"operation\":\"status\",\"credential\":123}") ==
            AlexaError::malformed);
    // credential empty.
    REQUIRE(parse_error("{\"version\":1,\"operation\":\"status\",\"credential\":\"\"}") ==
            AlexaError::malformed);
    // request_id wrong type.
    REQUIRE(parse_error("{\"version\":1,\"operation\":\"status\",\"credential\":\"t\","
                        "\"request_id\":5}") == AlexaError::malformed);
}

TEST_CASE("Alexa: client-supplied identity/authorization claims are ignored",
          "[alexa][safety]") {
    // A request that tries to assert its own principal or permissions must be
    // rejected as malformed (unknown fields) — authorization comes only from
    // the verifier, never from the request body.
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    // Unknown identity/permission fields => malformed, before any auth.
    REQUIRE(parse_error(spoofed_body("status", "tok-noperm")) == AlexaError::malformed);
    // Even a well-formed request from a no-permission principal is forbidden.
    auto r = AlexaContract::handle(parse_ok(valid_body("status", "tok-noperm")), v, p, now);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error == AlexaError::forbidden);
}

TEST_CASE("Alexa: credential value never appears in the response",
          "[alexa][safety]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    // Serialize a response and confirm the credential token is absent.
    auto r = AlexaContract::handle(parse_ok(valid_body("status", "tok-full")), v, p, now);
    REQUIRE(r.ok);
    // The response struct has no field carrying the credential; assert the
    // error message and state strings don't contain the token.
    std::string msg = AlexaContract::error_message(r.error);
    REQUIRE(msg.find("tok-full") == std::string::npos);
    REQUIRE(r.state.find("tok-full") == std::string::npos);
}

// ---------------------------------------------------------------------------
// devices operation: privacy-safe visibility, default-deny, and isolation.
// The owner's visibility model is not settled, so the behaviour that matters most
// is that NOTHING is visible without an explicit grant, and that a caller permitted
// for other operations cannot infer device visibility at all.
// ---------------------------------------------------------------------------

TEST_CASE("Alexa: devices is forbidden for a token that lacks the device permission",
          "[alexa][contract][devices]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    p.snap.state = "Running";
    p.snap.visible_device_labels = {"site-a-camera", "site-a-motion"};  // approved, and yet...

    // tok-full holds status + alert_summary. It must NOT reach device visibility: permission is
    // per operation, so this is forbidden even though devices ARE approved. That is default-deny.
    auto parsed = AlexaContract::parse(valid_body("devices", "tok-full"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.error == AlexaError::forbidden);
    REQUIRE(resp.device_labels.empty());
    REQUIRE(resp.device_count == 0);
}

TEST_CASE("Alexa: devices with the permission but no approved visibility reports zero",
          "[alexa][contract][devices]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    p.snap.state = "Running";
    // Default state: the provider has approved nothing.

    auto parsed = AlexaContract::parse(valid_body("devices", "tok-devices"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE(resp.ok);
    REQUIRE(resp.device_count == 0);
    REQUIRE(resp.device_labels.empty());
    // The marker distinguishes "nothing is approved as visible" from "there are no devices".
    REQUIRE(resp.device_visibility == "owner-approved-only");
}

TEST_CASE("Alexa: devices returns approved labels and caps the list",
          "[alexa][contract][devices]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    p.snap.state = "Running";
    p.snap.visible_device_labels = {"site-a-camera", "site-b-motion"};

    auto parsed = AlexaContract::parse(valid_body("devices", "tok-devices"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE(resp.ok);
    REQUIRE(resp.device_count == 2);
    REQUIRE(resp.device_labels.size() == 2);

    // More approved labels than the cap: the returned list is capped while the count still
    // reports the true total, so a viewer can tell "truncated" from "that is all of them".
    p.snap.visible_device_labels.clear();
    for (size_t i = 0; i < ALEXA_MAX_DEVICES_RETURNED + 3; ++i) {
        p.snap.visible_device_labels.push_back("label-" + std::to_string(i));
    }
    auto parsed2 = AlexaContract::parse(valid_body("devices", "tok-devices"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed2));
    AlexaResponse resp2 = AlexaContract::handle(std::get<AlexaRequest>(parsed2), v, p, now);
    REQUIRE(resp2.ok);
    REQUIRE(resp2.device_labels.size() == ALEXA_MAX_DEVICES_RETURNED);
    REQUIRE(resp2.device_count == ALEXA_MAX_DEVICES_RETURNED + 3);
}

TEST_CASE("Alexa: other operations never carry device visibility",
          "[alexa][contract][devices]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    p.snap.state = "Running";
    p.snap.alert_count = 1;
    p.snap.highest_severity = "warning";
    p.snap.alert_ids = {"alert-1"};
    p.snap.visible_device_labels = {"site-a-camera", "site-a-motion"};

    // Two devices are visible and this token may read status -- but the status response must not
    // disclose even the count, or device visibility would leak through the side door.
    auto parsed = AlexaContract::parse(valid_body("status", "tok-full"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE(resp.ok);
    REQUIRE(resp.device_count == 0);
    REQUIRE(resp.device_labels.empty());
    REQUIRE(resp.device_visibility.empty());

    auto parsed2 = AlexaContract::parse(valid_body("alert_summary", "tok-full"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed2));
    AlexaResponse resp2 = AlexaContract::handle(std::get<AlexaRequest>(parsed2), v, p, now);
    REQUIRE(resp2.ok);
    REQUIRE(resp2.device_count == 0);
    REQUIRE(resp2.device_labels.empty());
}

TEST_CASE("Alexa: the device permission does not grant anything else",
          "[alexa][contract][devices]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    FakeVerifier v = make_verifier(now);
    FakeProvider p;
    p.snap.available = true;
    p.snap.state = "Running";

    // The mirror of the first case: a device-permitted principal cannot read status.
    auto parsed = AlexaContract::parse(valid_body("status", "tok-devices"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed));
    AlexaResponse resp = AlexaContract::handle(std::get<AlexaRequest>(parsed), v, p, now);
    REQUIRE_FALSE(resp.ok);
    REQUIRE(resp.error == AlexaError::forbidden);

    auto parsed2 = AlexaContract::parse(valid_body("alert_summary", "tok-devices"));
    REQUIRE(std::holds_alternative<AlexaRequest>(parsed2));
    AlexaResponse resp2 = AlexaContract::handle(std::get<AlexaRequest>(parsed2), v, p, now);
    REQUIRE_FALSE(resp2.ok);
    REQUIRE(resp2.error == AlexaError::forbidden);
}

#pragma once

// AlexaContract: the application-facing integration contract for a future
// HomeGuardian Alexa adapter. This module defines and validates a small,
// VERSIONED, READ-ONLY request/response schema and its authentication
// boundary. It is pure domain logic: no network, no AWS, no deployed endpoint,
// no credentials. It CANNOT enable capture, grant consent, change
// configuration, or activate sensors — every operation it accepts is a read.
//
// The Alexa request handler (JavaScript skill side) depends only on this
// contract via an adapter; swapping a mock client for a real authenticated
// service later requires no change to the intent handlers.
//
// SECURITY MODEL:
//  - A client-supplied identity is NEVER trusted as proof of authorization.
//    Authorization requires a verifiable credential checked by an injected
//    verifier. The contract fails closed: missing, invalid, expired, or
//    unauthorized credentials are rejected with UNAUTHORIZED.
//  - Requests are bounded (max size, known fields only). Unknown fields,
//    unsupported operations, and malformed input are rejected.
//  - Errors are generic: no internal paths, stack details, or diagnostics leak
//    to the client. Responses contain only approved, non-sensitive summary
//    fields (state, capture flag, aggregate alert counts/severities). No media
//    payloads, secrets, or personal identifiers are ever returned.

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace homeguardian {

// Supported contract version. Requests with a different version are rejected
// with UNSUPPORTED_VERSION (never silently coerced).
inline constexpr int ALEXA_CONTRACT_VERSION = 1;

// Hard bounds (fail closed on anything larger).
inline constexpr size_t ALEXA_MAX_REQUEST_BYTES = 4096;
inline constexpr size_t ALEXA_MAX_TOKEN_CHARS = 512;
inline constexpr size_t ALEXA_MAX_ALERTS_RETURNED = 5;

// The only operations this contract exposes. All are reads. There is
// deliberately no operation that mutates capture, consent, config, or sensors.
enum class AlexaOperation {
    status,         // read-only lifecycle status
    alert_summary,  // read-only aggregate alert summary
};

// A parsed, validated request. Constructed only via AlexaContract::parse().
struct AlexaRequest {
    int version = 0;
    AlexaOperation operation{};
    std::string credential;   // presented credential (opaque token string)
    std::string request_id;   // caller-supplied correlation id (optional)
};

// An error category returned to the client. Values map to generic client
// messages; they never carry internal detail.
enum class AlexaError {
    none,
    malformed,          // bad JSON, missing/unknown fields, oversized
    unsupported_version,
    unauthorized,       // missing/invalid/expired credential
    forbidden,          // authenticated but not permitted for this operation
    unavailable,        // application degraded or unavailable
};

// The read-only response payload. Only approved, non-sensitive fields.
struct AlexaResponse {
    bool ok = false;
    AlexaError error = AlexaError::none;

    // status operation
    std::string state;          // e.g. "Running" / "Unavailable"
    bool capture_enabled = false;

    // alert_summary operation (aggregate only; never raw alerts)
    size_t alert_count = 0;
    std::string highest_severity;  // "" when none
    std::vector<std::string> alert_ids;  // capped, non-sensitive ids
};

// Authorization context produced by a successful credential check. Carries
// only what the contract needs: the authenticated principal and whether that
// principal may perform the requested operation.
struct AlexaAuthContext {
    bool authenticated = false;
    std::string principal;      // opaque identifier of the caller
    bool allow_status = false;
    bool allow_alert_summary = false;
};

// Credential verifier interface. The production implementation would validate a
// real token (e.g. an account-linking token or a signed request). Tests inject
// deterministic fakes. It MUST NOT trust a client-supplied identity as proof:
// it returns an auth context only after verifying the credential.
class IAlexaCredentialVerifier {
public:
    virtual ~IAlexaCredentialVerifier() = default;
    // Verify `credential` at `now`. Return an auth context; on any failure
    // (missing, invalid, expired, unknown) return a context with
    // authenticated=false. Must be side-effect free and never throw for a bad
    // credential.
    virtual AlexaAuthContext verify(const std::string& credential,
                                    std::chrono::system_clock::time_point now) const = 0;
};

// The application-side data provider. Read-only snapshot source. Injected so
// the contract never touches persistence, media, or configuration directly.
struct AlexaStatusSnapshot {
    bool available = true;      // false => application degraded/unavailable
    std::string state;          // lifecycle state string
    bool capture_enabled = false;
    size_t alert_count = 0;
    std::string highest_severity;
    std::vector<std::string> alert_ids;
};

class IAlexaStatusProvider {
public:
    virtual ~IAlexaStatusProvider() = default;
    virtual AlexaStatusSnapshot snapshot() const = 0;
};

class AlexaContract {
public:
    // Parse and fully validate a raw request body (JSON text). Returns the
    // request on success. On failure returns an AlexaError describing why.
    // Never throws; never trusts unvalidated input.
    static std::variant<AlexaRequest, AlexaError> parse(const std::string& body);

    // Handle a parsed request: authenticate, authorize, and (if permitted and
    // the app is available) produce a read-only response. Fails closed at every
    // step: unauthorized, forbidden, unsupported version, or unavailable all
    // return an error response — never partial or sensitive data.
    static AlexaResponse handle(const AlexaRequest& request,
                                const IAlexaCredentialVerifier& verifier,
                                const IAlexaStatusProvider& provider,
                                std::chrono::system_clock::time_point now =
                                    std::chrono::system_clock::now());

    // Generic, non-sensitive client message for an error category.
    static std::string error_message(AlexaError err);
};

}  // namespace homeguardian

#include "AlexaContract.h"

#include <nlohmann/json.hpp>

namespace homeguardian {

using json = nlohmann::json;

namespace {

// Field-name limits: a request body may only contain these known fields. Any
// other field is rejected (malformed) rather than ignored, so a future field
// cannot silently change semantics.
bool is_known_top_level_field(const std::string& key) {
    return key == "version" || key == "operation" || key == "credential" ||
           key == "request_id";
}

// Validate a credential string: non-empty and within the size bound. The
// contract does NOT interpret the credential's contents; the verifier does.
bool credential_shape_ok(const std::string& cred) {
    return !cred.empty() && cred.size() <= ALEXA_MAX_TOKEN_CHARS;
}

// Optional request_id must be a bounded, control-character-free string.
bool request_id_shape_ok(const std::string& id) {
    if (id.size() > 128) return false;
    for (char c : id) {
        if (static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

}  // namespace

std::variant<AlexaRequest, AlexaError> AlexaContract::parse(const std::string& body) {
    // 1. Size bound: reject anything oversized before parsing.
    if (body.empty() || body.size() > ALEXA_MAX_REQUEST_BYTES) {
        return AlexaError::malformed;
    }

    // 2. Strict JSON parse. Trailing data / non-object => malformed.
    json j;
    try {
        j = json::parse(body);
    } catch (const json::parse_error&) {
        return AlexaError::malformed;
    }
    if (!j.is_object()) {
        return AlexaError::malformed;
    }

    // 3. Reject unknown fields (fail closed on schema drift).
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!is_known_top_level_field(it.key())) {
            return AlexaError::malformed;
        }
    }

    // 4. Version must be an integer equal to the supported version. A different
    //    version is a distinct error (not coerced).
    if (!j.contains("version") || !j["version"].is_number_integer()) {
        return AlexaError::malformed;
    }
    int version = j["version"].get<int>();
    if (version != ALEXA_CONTRACT_VERSION) {
        return AlexaError::unsupported_version;
    }

    // 5. Operation must be one of the supported read-only operations.
    if (!j.contains("operation") || !j["operation"].is_string()) {
        return AlexaError::malformed;
    }
    std::string op = j["operation"].get<std::string>();
    AlexaOperation operation{};
    if (op == "status") {
        operation = AlexaOperation::status;
    } else if (op == "alert_summary") {
        operation = AlexaOperation::alert_summary;
    } else {
        // Unknown operation => malformed (not an auth failure; the request
        // itself is invalid).
        return AlexaError::malformed;
    }

    // 6. Credential must be present and well-shaped. We do not trust its
    //    contents here; the verifier decides authorization.
    if (!j.contains("credential") || !j["credential"].is_string()) {
        return AlexaError::malformed;
    }
    std::string credential = j["credential"].get<std::string>();
    if (!credential_shape_ok(credential)) {
        return AlexaError::malformed;
    }

    // 7. request_id is optional but bounded if present.
    std::string request_id;
    if (j.contains("request_id")) {
        if (!j["request_id"].is_string()) return AlexaError::malformed;
        request_id = j["request_id"].get<std::string>();
        if (!request_id_shape_ok(request_id)) return AlexaError::malformed;
    }

    AlexaRequest req;
    req.version = version;
    req.operation = operation;
    req.credential = std::move(credential);
    req.request_id = std::move(request_id);
    return req;
}

AlexaResponse AlexaContract::handle(const AlexaRequest& request,
                                    const IAlexaCredentialVerifier& verifier,
                                    const IAlexaStatusProvider& provider,
                                    std::chrono::system_clock::time_point now) {
    AlexaResponse resp;

    // 1. Version gate (defense in depth; parse already checks this).
    if (request.version != ALEXA_CONTRACT_VERSION) {
        resp.error = AlexaError::unsupported_version;
        return resp;
    }

    // 2. Authenticate. A client-supplied identity is never trusted: only a
    //    verified credential yields an authenticated context. Missing/invalid/
    //    expired credentials fail closed.
    AlexaAuthContext auth = verifier.verify(request.credential, now);
    if (!auth.authenticated) {
        resp.error = AlexaError::unauthorized;
        return resp;
    }

    // 3. Authorize the specific operation. Authentication alone is not enough;
    //    the principal must be permitted for THIS operation.
    bool permitted = (request.operation == AlexaOperation::status && auth.allow_status) ||
                     (request.operation == AlexaOperation::alert_summary &&
                      auth.allow_alert_summary);
    if (!permitted) {
        resp.error = AlexaError::forbidden;
        return resp;
    }

    // 4. Application availability. If the app is degraded/unavailable, fail
    //    closed rather than returning stale or partial state.
    AlexaStatusSnapshot snap = provider.snapshot();
    if (!snap.available) {
        resp.error = AlexaError::unavailable;
        return resp;
    }

    // 5. Produce the read-only response with only approved, non-sensitive
    //    fields. Alert ids are capped and contain no media or personal data.
    resp.ok = true;
    resp.error = AlexaError::none;
    resp.state = snap.state;
    resp.capture_enabled = snap.capture_enabled;
    resp.alert_count = snap.alert_count;
    resp.highest_severity = snap.highest_severity;
    size_t n = snap.alert_ids.size() < ALEXA_MAX_ALERTS_RETURNED
                   ? snap.alert_ids.size()
                   : ALEXA_MAX_ALERTS_RETURNED;
    resp.alert_ids.assign(snap.alert_ids.begin(), snap.alert_ids.begin() + n);
    return resp;
}

std::string AlexaContract::error_message(AlexaError err) {
    switch (err) {
        case AlexaError::none: return "ok";
        case AlexaError::malformed: return "The request was malformed.";
        case AlexaError::unsupported_version: return "Unsupported request version.";
        case AlexaError::unauthorized: return "The request was not authorized.";
        case AlexaError::forbidden: return "The request was not permitted.";
        case AlexaError::unavailable: return "The service is temporarily unavailable.";
    }
    return "The request could not be completed.";
}

}  // namespace homeguardian

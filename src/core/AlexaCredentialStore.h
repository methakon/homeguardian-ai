#pragma once

// AlexaCredentialStore: a hardened, provider-independent bearer-token verifier
// for the Alexa integration boundary. It implements the REAL credential
// verification and secure-secret-handling layer (security blockers B1 + B4 from
// docs/SECURITY_READINESS_V5.md).
//
// WHAT THIS IS:
//  - A local verifier that validates opaque bearer tokens against a securely
//    provisioned credential file. Each entry carries a principal, per-operation
//    permissions, an optional expiry, and a revocation flag.
//  - Fail-closed: missing/unreadable/malformed/insecurely-permissioned files,
//    unknown/expired/revoked tokens all yield authenticated=false. There is NO
//    permissive fallback: if the store cannot be loaded securely, every verify
//    fails.
//  - Token comparison is length-checked and constant-time over the token bytes
//    (no early exit on first mismatch) to avoid leaking validity by timing.
//
// WHAT THIS IS NOT:
//  - NOT a specific identity provider. The repository documents the intended
//    production flow as Alexa account linking via OAuth 2.0 / Login with Amazon
//    (docs/ARCHITECTURE.md). Validating an LWA access token requires an LWA
//    client id/secret (Amazon credentials), which this milestone does not have
//    and must not fabricate. This module is the PROVIDER-INDEPENDENT boundary:
//    a future LWA/signed-request verifier implements the same
//    IAlexaCredentialVerifier interface and replaces this store at the wiring
//    point without touching AlexaContract semantics.
//  - NOT a secret generator or a production secret store. Provisioning,
//    rotation, and revocation of real secrets are operational procedures
//    (documented); this module only ENFORCES secure loading and verification.
//
// SECRET HANDLING:
//  - The credential file must be owner-only readable (0600) and owned by the
//    current effective uid; a broader mode or different owner is REJECTED
//    (fail closed) rather than silently used.
//  - Secret values are never logged, never included in error messages, and
//    never returned. Test fixtures are synthetic and isolated.
//  - Production secrets live outside Git and ordinary application config; the
//    file path is supplied at runtime, never embedded.

#include "core/AlexaContract.h"  // IAlexaCredentialVerifier, AlexaAuthContext

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

// One credential entry. `token` is the opaque bearer secret; it is never
// logged or returned. `revoked` and `expires_at` provide lifecycle control.
struct AlexaCredential {
    std::string principal;
    std::string credential_id;
    bool allow_status = false;
    bool allow_alert_summary = false;
    std::chrono::system_clock::time_point expires_at{};  // max() = never
    bool revoked = false;
};

// Result of loading a credential store. On failure the store is empty and
// `ok` is false; the caller MUST treat every verification as failed.
struct AlexaCredentialStoreLoadResult {
    bool ok = false;
    std::string reason;  // generic, non-sensitive reason (no paths/values)
    size_t entry_count = 0;
};

class AlexaCredentialStore : public IAlexaCredentialVerifier {
public:
    AlexaCredentialStore() = default;

    // Securely load credentials from `path`. Fails closed unless the file
    // exists, is a regular file, is owner-only (0600), is owned by the current
    // effective uid, and parses as the expected JSON schema. On ANY failure the
    // store stays empty and the returned result explains why (generically).
    // Secret values are never included in `reason`.
    AlexaCredentialStoreLoadResult load(const std::string& path);

    // IAlexaCredentialVerifier: verify a presented bearer token at `now`.
    // Returns authenticated=true ONLY for a known, unrevoked, unexpired token.
    // Never throws; never leaks whether a token exists vs is expired vs is
    // revoked (all failures are indistinguishable to the caller).
    AlexaAuthContext verify(const std::string& credential,
                            std::chrono::system_clock::time_point now) const override;

    // Introspection (no secret values exposed).
    size_t size() const { return entries_.size(); }
    bool loaded_ok() const { return loaded_ok_; }

    // Revoke a token at runtime (in-memory). Returns true if it existed.
    // This is an operational control for the running process; persistent
    // revocation is a provisioning procedure (documented).
    bool revoke(const std::string& credential);

    // Programmatic construction for tests (synthetic fixtures only). Bypasses
    // the secure-file path; callers must never use this for production secrets.
    void add_for_testing(const std::string& token, const AlexaCredential& entry);

private:
    struct Entry {
        std::string token;       // opaque secret; never logged/returned
        AlexaCredential cred;
    };

    // Constant-time-ish equality for equal-length byte strings: compare every
    // byte and accumulate differences so timing does not reveal the first
    // mismatching position. Length mismatch fails immediately (length is not
    // secret). No cryptographic hash is used because this milestone adds no new
    // dependency or custom crypto; see module comment.
    static bool equal_bytes(const std::string& a, const std::string& b);

    std::vector<Entry> entries_;
    bool loaded_ok_ = false;
};

}  // namespace homeguardian

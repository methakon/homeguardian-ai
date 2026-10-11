#include "AlexaCredentialStore.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

namespace homeguardian {

using json = nlohmann::json;

AlexaCredentialStoreLoadResult AlexaCredentialStore::load(const std::string& path) {
    AlexaCredentialStoreLoadResult result;
    entries_.clear();
    loaded_ok_ = false;

    // --- Secure file checks (fail closed on any problem) ---
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        result.reason = "credential file not accessible";
        return result;
    }
    if (!S_ISREG(st.st_mode)) {
        result.reason = "credential path is not a regular file";
        return result;
    }
    // Owner-only readable: no group/other bits may be set.
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        result.reason = "credential file permissions too broad (require 0600)";
        return result;
    }
    // Owned by the current effective uid (no shared/foreign-owned secrets).
    if (st.st_uid != ::geteuid()) {
        result.reason = "credential file not owned by current user";
        return result;
    }

    // --- Read + parse (secret values never logged) ---
    std::ifstream f(path);
    if (!f.is_open()) {
        result.reason = "credential file could not be opened";
        return result;
    }
    json j;
    try {
        f >> j;
    } catch (const json::parse_error&) {
        result.reason = "credential file is not valid JSON";
        return result;
    }
    if (!j.is_object()) {
        result.reason = "credential file must be a JSON object";
        return result;
    }

    // Schema: { "token": { "principal": str, "allow_status": bool,
    //                       "allow_alert_summary": bool,
    //                       "expires_at_ms": int (0 = never),
    //                       "revoked": bool } , ... }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& token = it.key();
        const json& val = it.value();
        if (!val.is_object()) {
            result.reason = "credential entry must be an object";
            entries_.clear();
            return result;
        }
        Entry e;
        e.token = token;
        if (token.empty()) {
            result.reason = "credential token must be non-empty";
            entries_.clear();
            return result;
        }
        if (val.contains("principal") && val["principal"].is_string()) {
            e.cred.principal = val["principal"].get<std::string>();
        }
        if (val.contains("allow_status") && val["allow_status"].is_boolean()) {
            e.cred.allow_status = val["allow_status"].get<bool>();
        }
        if (val.contains("allow_alert_summary") && val["allow_alert_summary"].is_boolean()) {
            e.cred.allow_alert_summary = val["allow_alert_summary"].get<bool>();
        }
        if (val.contains("revoked") && val["revoked"].is_boolean()) {
            e.cred.revoked = val["revoked"].get<bool>();
        }
        long long exp = 0;
        if (val.contains("expires_at_ms") && val["expires_at_ms"].is_number_integer()) {
            exp = val["expires_at_ms"].get<long long>();
        }
        e.cred.expires_at = (exp <= 0)
            ? std::chrono::system_clock::time_point::max()
            : std::chrono::system_clock::time_point(std::chrono::milliseconds(exp));
        entries_.push_back(std::move(e));
    }

    loaded_ok_ = true;
    result.ok = true;
    result.entry_count = entries_.size();
    return result;
}

AlexaAuthContext AlexaCredentialStore::verify(const std::string& credential,
                                              std::chrono::system_clock::time_point now) const {
    AlexaAuthContext ctx;  // defaults: authenticated=false (fail closed)
    if (credential.empty()) return ctx;

    // Scan every entry and use constant-time comparison so that neither the
    // match position nor an early mismatch is observable by timing. All
    // failure modes (unknown / revoked / expired) are indistinguishable.
    const Entry* match = nullptr;
    for (const auto& e : entries_) {
        if (equal_bytes(credential, e.token) && match == nullptr) {
            match = &e;
            // Do NOT break: keep scanning so total work does not reveal which
            // entry (if any) matched.
        }
    }
    if (match == nullptr) return ctx;              // unknown token
    if (match->cred.revoked) return ctx;           // revoked
    if (now >= match->cred.expires_at) return ctx;  // expired

    ctx.authenticated = true;
    ctx.principal = match->cred.principal;
    ctx.allow_status = match->cred.allow_status;
    ctx.allow_alert_summary = match->cred.allow_alert_summary;
    return ctx;
}

bool AlexaCredentialStore::revoke(const std::string& credential) {
    for (auto& e : entries_) {
        if (equal_bytes(credential, e.token)) {
            e.cred.revoked = true;
            return true;
        }
    }
    return false;
}

void AlexaCredentialStore::add_for_testing(const std::string& token,
                                           const AlexaCredential& entry) {
    AlexaCredential normalized = entry;
    // A default-constructed expires_at (epoch) would read as already-expired.
    // For test fixtures, treat a zero/epoch expiry as "never expires" so the
    // caller does not have to set max() explicitly. (The secure file loader
    // already maps expires_at_ms<=0 to max(); this keeps the two paths
    // consistent.)
    if (normalized.expires_at == std::chrono::system_clock::time_point{}) {
        normalized.expires_at = std::chrono::system_clock::time_point::max();
    }
    entries_.push_back(Entry{token, std::move(normalized)});
}

bool AlexaCredentialStore::equal_bytes(const std::string& a, const std::string& b) {
    // Length is not secret; a length mismatch is an immediate (non-timing-
    // sensitive) negative. For equal lengths, accumulate XOR differences across
    // ALL bytes so the comparison time does not depend on where they differ.
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

}  // namespace homeguardian

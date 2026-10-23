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

namespace {

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t choose(uint32_t e, uint32_t f, uint32_t g) {
    return (e & f) ^ (~e & g);
}

inline uint32_t majority(uint32_t a, uint32_t b, uint32_t c) {
    return (a & b) ^ (a & c) ^ (b & c);
}

inline uint32_t sig0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sig1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t theta0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t theta1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

std::string compute_sha256_hex(const std::string& input) {
    uint32_t H[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    uint64_t total_bits = static_cast<uint64_t>(input.size()) * 8;
    std::vector<uint8_t> msg(input.begin(), input.end());
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((total_bits >> (i * 8)) & 0xff));
    }

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t W[64];
        for (int t = 0; t < 16; ++t) {
            W[t] = (static_cast<uint32_t>(msg[chunk + t * 4]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + t * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + t * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(msg[chunk + t * 4 + 3]));
        }
        for (int t = 16; t < 64; ++t) {
            W[t] = theta1(W[t - 2]) + W[t - 7] + theta0(W[t - 15]) + W[t - 16];
        }

        uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
        uint32_t e = H[4], f = H[5], g = H[6], h = H[7];

        for (int t = 0; t < 64; ++t) {
            uint32_t T1 = h + sig1(e) + choose(e, f, g) + K256[t] + W[t];
            uint32_t T2 = sig0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + T1;
            d = c;
            c = b;
            b = a;
            a = T1 + T2;
        }

        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }

    char buf[65];
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x%08x%08x%08x%08x",
                  H[0], H[1], H[2], H[3], H[4], H[5], H[6], H[7]);
    return std::string(buf);
}

}  // namespace

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
    //                       "allow_devices": bool,      (absent => false)
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
        e.cred.credential_id = compute_sha256_hex(token);

        if (val.contains("principal")) {
            if (!val["principal"].is_string()) {
                result.reason = "credential principal must be a string";
                entries_.clear();
                return result;
            }
            std::string p = val["principal"].get<std::string>();
            if (p.empty()) {
                result.reason = "credential principal must be non-empty";
                entries_.clear();
                return result;
            }
            e.cred.principal = std::move(p);
        } else {
            // Default credential identity derived from hash when principal is omitted
            e.cred.principal = "cred:" + e.cred.credential_id.substr(0, 16);
        }

        if (val.contains("allow_status") && val["allow_status"].is_boolean()) {
            e.cred.allow_status = val["allow_status"].get<bool>();
        }
        if (val.contains("allow_alert_summary") && val["allow_alert_summary"].is_boolean()) {
            e.cred.allow_alert_summary = val["allow_alert_summary"].get<bool>();
        }
        if (val.contains("allow_devices") && val["allow_devices"].is_boolean()) {
            e.cred.allow_devices = val["allow_devices"].get<bool>();
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
    ctx.credential_id = match->cred.credential_id;
    ctx.allow_status = match->cred.allow_status;
    ctx.allow_alert_summary = match->cred.allow_alert_summary;
    ctx.allow_devices = match->cred.allow_devices;
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
    if (normalized.credential_id.empty()) {
        normalized.credential_id = compute_sha256_hex(token);
    }
    if (normalized.principal.empty()) {
        normalized.principal = "cred:" + normalized.credential_id.substr(0, 16);
    }
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

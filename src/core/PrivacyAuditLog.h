#pragma once

// PrivacyAuditLog (Runtime Readiness v13, roadmap H-03).
//
// A bounded, thread-safe, METADATA-ONLY audit log of consent-gated data-access
// decisions. Its purpose is the H-03 acceptance criterion: "all data access
// logged; consent enforcement verified".
//
// WHAT IT RECORDS: one entry per data-access decision — the scope, the purpose,
// the data category, whether the gate authorized it, the deny reason (when
// denied), and whether a payload was actually acquired. These are descriptive
// facts about a *decision*, not about a person or their media.
//
// WHAT IT NEVER RECORDS: raw audio, raw video, frames, samples, transcripts,
// facial embeddings, biometric templates, or any media payload. There is no
// field for them and none is derived. String fields are length-clamped so a
// malformed caller cannot bloat an entry.
//
// SAFETY: the log is a passive observer at the delivery boundary. It does NOT
// make or change any decision — recording is best-effort and never throws, so a
// logging failure can never widen access or crash the runtime. "Consent
// enforcement verified" is exposed as an invariant (verify_enforcement): no
// entry may record an acquisition that was not authorized.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace homeguardian {

struct PrivacyAuditEntry {
    std::chrono::system_clock::time_point timestamp{};
    std::string subject_scope;    // household/profile scope, e.g. "harness"
    std::string purpose;          // e.g. "presence", "fall_detection"
    std::string data_category;    // e.g. "audio", "camera", "sensor"
    bool authorized = false;      // gate decision
    std::string decision_reason;  // "none" when authorized; else the deny reason
    bool acquired = false;        // true only when a payload was produced AND authorized
};

class PrivacyAuditLog {
public:
    explicit PrivacyAuditLog(size_t capacity = 256);

    // Append one decision. Bounded (oldest evicted past `capacity`), thread-safe,
    // and never throws — a logging failure must not affect the runtime.
    void record(const PrivacyAuditEntry& entry);

    void clear();

    struct Counts {
        uint64_t total = 0;
        uint64_t authorized = 0;
        uint64_t denied = 0;
        uint64_t acquired = 0;
    };

    Counts counts() const;
    size_t size() const;
    size_t capacity() const { return capacity_; }

    // The most recent `n` entries, oldest-first (fewer if not that many exist).
    std::vector<PrivacyAuditEntry> recent(size_t n) const;

    // The H-03 invariant: no entry may record an acquisition that was not
    // authorized. Returns true when consent enforcement is upheld across every
    // retained entry.
    bool verify_enforcement() const;

    // Longest string field retained per entry (excess is truncated).
    static constexpr size_t kMaxFieldChars = 128;
    static std::string clamp_field(const std::string& s);

private:
    size_t capacity_;
    mutable std::mutex m_;
    std::deque<PrivacyAuditEntry> entries_;
    Counts counts_;
};

} // namespace homeguardian

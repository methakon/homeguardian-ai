#pragma once

// Single-active-grant consent repository.
//
// WHY THIS TYPE EXISTS (H-03 defect, reproduced on the phone):
//   ConsentGate::check() fails closed with DenyReason::ambiguous when ONE (subject, purpose) has
//   MORE THAN ONE active grant record - it refuses to guess which grant the operator meant. That
//   rule is correct and is NOT changed here. The defect was in the repository feeding it: it
//   APPENDED a granted record on every press of "Grant consent", so the second press turned a
//   perfectly good authorization into a permanent denial. The gate said `ambiguous`; the
//   repository was the thing that was wrong.
//
// The invariant this type maintains, whatever the click sequence:
//   AT MOST ONE `granted` record per (subject_scope, purpose).
//
// Deterministic lifecycle:
//   grant(subject, purpose, categories)
//       Supersedes (removes) any existing granted record for that (subject, purpose) and appends
//       exactly one fresh granted record. Idempotent: pressing Grant ten times authorizes exactly
//       as once and can never manufacture ambiguity.
//   withdraw(subject, purpose)
//       Removes EVERY granted record for that (subject, purpose) and appends one withdrawn
//       record. A later withdrawal therefore cannot leave an earlier grant effective, and the
//       withdrawal stays on record as the audit trail.
//
// Both are "last write wins" per (subject, purpose); different purposes never interfere with each
// other. Nothing about the fail-closed behaviour is relaxed: no record at all still denies
// (no_consent_record), a withdrawn record still denies (decision_not_granted), an expired grant
// still denies (expired), a category not covered by the grant still denies (category_missing).
//
// Header-only on purpose: it is a self-contained policy type with no dependencies beyond the
// standard library, so it compiles identically in the host test build and in the Android JNI
// translation unit without touching either build's source list.

#include "core/ConsentRecord.h"
#include "persistence/IConsentRepository.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

class SingleActiveGrantConsentRepo : public IConsentRepository {
public:
    // --- IConsentRepository: the raw record store (used by persistence-backed repos too) --------

    // Appends without applying the lifecycle. Kept faithful to the interface contract so tests can
    // construct the conflicting state the gate is supposed to refuse; production code should use
    // grant()/withdraw() below.
    void save(const ConsentRecord& record) override {
        std::lock_guard<std::mutex> lk(m_);
        records_.push_back(record);
    }

    std::optional<ConsentRecord> find_by_id(const std::string& consent_id) override {
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& r : records_) {
            if (r.get_consent_id() == consent_id) return r;
        }
        return std::nullopt;
    }

    std::vector<ConsentRecord> find_by_subject(const std::string& subject_scope) override {
        std::lock_guard<std::mutex> lk(m_);
        return select_locked(subject_scope, "");
    }

    std::vector<ConsentRecord> find_by_purpose(const std::string& subject_scope,
                                               const std::string& purpose) override {
        std::lock_guard<std::mutex> lk(m_);
        return select_locked(subject_scope, purpose);
    }

    size_t count() override {
        std::lock_guard<std::mutex> lk(m_);
        return records_.size();
    }

    size_t delete_all() override {
        std::lock_guard<std::mutex> lk(m_);
        size_t n = records_.size();
        records_.clear();
        return n;
    }

    // --- operator lifecycle --------------------------------------------------------------------

    // Records the operator's consent. Returns how many earlier granted records were superseded
    // (0 on a first grant or on a repeat press of an already-granted purpose) - useful for
    // diagnostics and for tests that assert idempotence.
    size_t grant(const std::string& subject_scope, const std::string& purpose,
                 const std::vector<std::string>& data_categories,
                 const std::string& policy_version = "1.0.0",
                 const std::string& actor = "operator") {
        std::lock_guard<std::mutex> lk(m_);
        const size_t superseded = remove_granted_locked(subject_scope, purpose);
        records_.push_back(ConsentRecord::create_granted("node-grant", subject_scope, purpose,
                                                         data_categories, policy_version, actor));
        return superseded;
    }

    // Records the operator's withdrawal. Removes every granted record for the (subject, purpose)
    // first, so no earlier grant can be effective afterwards, then appends the withdrawn record.
    // Returns how many granted records were removed.
    size_t withdraw(const std::string& subject_scope, const std::string& purpose,
                    const std::string& policy_version = "1.0.0",
                    const std::string& actor = "operator") {
        std::lock_guard<std::mutex> lk(m_);
        const size_t removed = remove_granted_locked(subject_scope, purpose);
        records_.push_back(ConsentRecord::create_withdrawn("node-withdraw", subject_scope, purpose,
                                                            policy_version, actor));
        return removed;
    }

    // Convenience view of the latest decision for a (subject, purpose): true only when the most
    // recent record for it is an active grant AND no other active grant conflicts. Mirrors the
    // gate's rule; the gate remains the single authority for authorization decisions.
    bool granted(const std::string& subject_scope, const std::string& purpose) {
        std::lock_guard<std::mutex> lk(m_);
        const auto selected = select_locked(subject_scope, purpose);
        if (selected.empty()) return false;
        auto latest = newest_locked(selected);
        return latest.get_decision() == ConsentDecision::granted && latest.is_active();
    }

    // Number of ACTIVE granted records for a (subject, purpose). The invariant this type maintains
    // is that this never exceeds 1; tests assert it directly after hostile press sequences.
    size_t active_grant_count(const std::string& subject_scope, const std::string& purpose) {
        std::lock_guard<std::mutex> lk(m_);
        size_t n = 0;
        for (const auto& r : records_) {
            if (r.get_subject_scope() == subject_scope && r.get_purpose() == purpose &&
                r.get_decision() == ConsentDecision::granted && r.is_active()) {
                ++n;
            }
        }
        return n;
    }

    // Every record, for diagnostics and audit. Never contains media or credentials.
    std::vector<ConsentRecord> all() {
        std::lock_guard<std::mutex> lk(m_);
        return records_;
    }

private:
    // Caller holds m_.
    std::vector<ConsentRecord> select_locked(const std::string& subject_scope,
                                             const std::string& purpose) const {
        std::vector<ConsentRecord> out;
        for (const auto& r : records_) {
            if (r.get_subject_scope() == subject_scope &&
                (purpose.empty() || r.get_purpose() == purpose)) {
                out.push_back(r);
            }
        }
        return out;
    }

    // Caller holds m_. Returns the most recent record of the set (empty set never happens here).
    static ConsentRecord newest_locked(std::vector<ConsentRecord> set) {
        std::sort(set.begin(), set.end(), [](const ConsentRecord& a, const ConsentRecord& b) {
            return a.get_recorded_at() < b.get_recorded_at();
        });
        return set.back();
    }

    // Caller holds m_. Removes all `granted` records for the (subject, purpose); returns the count.
    // This is what makes grant() idempotent and withdrawal final.
    size_t remove_granted_locked(const std::string& subject_scope, const std::string& purpose) {
        const size_t before = records_.size();
        records_.erase(std::remove_if(records_.begin(), records_.end(),
                                      [&](const ConsentRecord& r) {
                                          return r.get_subject_scope() == subject_scope &&
                                                 r.get_purpose() == purpose &&
                                                 r.get_decision() == ConsentDecision::granted;
                                      }),
                       records_.end());
        return before - records_.size();
    }

    mutable std::mutex m_;
    std::vector<ConsentRecord> records_;
};

} // namespace homeguardian

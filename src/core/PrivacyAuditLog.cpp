#include "PrivacyAuditLog.h"

namespace homeguardian {

std::string PrivacyAuditLog::clamp_field(const std::string& s) {
    return s.size() <= kMaxFieldChars ? s : s.substr(0, kMaxFieldChars);
}

PrivacyAuditLog::PrivacyAuditLog(size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void PrivacyAuditLog::record(const PrivacyAuditEntry& entry) {
    try {
        std::lock_guard<std::mutex> lk(m_);
        PrivacyAuditEntry e = entry;
        e.subject_scope = clamp_field(e.subject_scope);
        e.purpose = clamp_field(e.purpose);
        e.data_category = clamp_field(e.data_category);
        e.decision_reason = clamp_field(e.decision_reason);
        if (entries_.size() >= capacity_) {
            entries_.pop_front();
        }
        entries_.push_back(std::move(e));
        counts_.total++;
        if (entry.authorized) {
            counts_.authorized++;
        } else {
            counts_.denied++;
        }
        if (entry.acquired) {
            counts_.acquired++;
        }
    } catch (...) {
        // Best-effort logging: a failure here must never affect the runtime or
        // widen access. Swallow and continue.
    }
}

void PrivacyAuditLog::clear() {
    std::lock_guard<std::mutex> lk(m_);
    entries_.clear();
    counts_ = Counts{};
}

PrivacyAuditLog::Counts PrivacyAuditLog::counts() const {
    std::lock_guard<std::mutex> lk(m_);
    return counts_;
}

size_t PrivacyAuditLog::size() const {
    std::lock_guard<std::mutex> lk(m_);
    return entries_.size();
}

std::vector<PrivacyAuditEntry> PrivacyAuditLog::recent(size_t n) const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<PrivacyAuditEntry> out;
    const size_t start = entries_.size() > n ? entries_.size() - n : 0;
    out.reserve(entries_.size() - start);
    for (size_t i = start; i < entries_.size(); ++i) {
        out.push_back(entries_[i]);
    }
    return out;
}

bool PrivacyAuditLog::verify_enforcement() const {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& e : entries_) {
        if (e.acquired && !e.authorized) {
            return false;  // data access without consent — invariant violated
        }
    }
    return true;
}

} // namespace homeguardian

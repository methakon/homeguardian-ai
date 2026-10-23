#include "AlertPolicy.h"

#include <algorithm>

namespace homeguardian {

AlertPolicy::AlertPolicy(std::chrono::milliseconds cooldown_ms,
                         size_t max_tracked_keys,
                         Clock clock)
    : cooldown_ms_(cooldown_ms.count() < 0 ? std::chrono::milliseconds(0) : cooldown_ms),
      max_tracked_keys_(max_tracked_keys == 0 ? 1 : max_tracked_keys),
      clock_(std::move(clock)) {}

std::string AlertPolicy::key_for(const Alert& alert) const {
    // Identity = rule_name + severity. Different rules or severities are
    // independent and never suppressed against each other.
    std::string sev;
    switch (alert.severity) {
        case Severity::info: sev = "info"; break;
        case Severity::warning: sev = "warning"; break;
        case Severity::critical: sev = "critical"; break;
    }
    return alert.rule_name + "|" + sev;
}

void AlertPolicy::evict_if_needed() {
    // Called with m_ held. Bound retained state: when over capacity, evict the
    // oldest-inserted keys. This caps memory regardless of how many distinct
    // conditions occur.
    while (state_.size() > max_tracked_keys_ && !insertion_order_.empty()) {
        const std::string oldest = insertion_order_.front();
        insertion_order_.erase(insertion_order_.begin());
        state_.erase(oldest);
    }
}

bool AlertPolicy::admit(const Alert& alert, bool& suppressed) {
    return admit_durable(alert, suppressed, [] { return true; });
}

bool AlertPolicy::admit_durable(const Alert& alert, bool& suppressed, const std::function<bool()>& commit) {
    std::lock_guard<std::mutex> lk(m_);
    suppressed = false;

    const std::string key = key_for(alert);
    auto now = clock_();

    auto it = state_.find(key);
    if (it == state_.end()) {
        // First time we've seen this key: always deliver (fresh condition).
        if (!commit()) return false;
        KeyState st;
        st.last_delivered = now;
        st.active = true;
        st.order = next_order_++;
        state_.emplace(key, st);
        insertion_order_.push_back(key);
        evict_if_needed();
        delivered_count_++;
        return true;
    }

    KeyState& st = it->second;

    if (!st.active) {
        if (!commit()) return false;
        // Condition had cleared and now re-fired: deliver immediately (fresh).
        st.active = true;
        st.last_delivered = now;
        delivered_count_++;
        return true;
    }

    // Condition is still active. Apply cooldown. Guard against a clock that
    // moves backward: a negative elapsed cannot extend the cooldown.
    auto elapsed = now - st.last_delivered;
    bool cooldown_expired =
        (elapsed >= cooldown_ms_) ||
        (elapsed < std::chrono::system_clock::duration::zero());

    if (cooldown_expired) {
        if (!commit()) return false;
        // Re-notification after cooldown (or clock went backward): deliver.
        st.last_delivered = now;
        delivered_count_++;
        return true;
    }

    // Within cooldown and still active: suppress this duplicate.
    suppressed = true;
    suppressed_count_++;
    return false;
}

void AlertPolicy::mark_condition_cleared(const std::string& rule_name) {
    std::lock_guard<std::mutex> lk(m_);
    // Mark all keys for this rule_name inactive so their next alert is a fresh
    // condition (delivered immediately). Keys are "rule_name|severity".
    for (auto& kv : state_) {
        const std::string::size_type sep = kv.first.find('|');
        const std::string rule = (sep == std::string::npos) ? kv.first : kv.first.substr(0, sep);
        if (rule == rule_name) {
            kv.second.active = false;
        }
    }
}

size_t AlertPolicy::suppressed_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return suppressed_count_;
}

size_t AlertPolicy::delivered_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return delivered_count_;
}

size_t AlertPolicy::tracked_keys() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_.size();
}

void AlertPolicy::reset() {
    std::lock_guard<std::mutex> lk(m_);
    state_.clear();
    insertion_order_.clear();
    suppressed_count_ = 0;
    delivered_count_ = 0;
    next_order_ = 0;
}

}  // namespace homeguardian

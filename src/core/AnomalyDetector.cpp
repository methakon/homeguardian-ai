#include "AnomalyDetector.h"

#include <cmath>
#include <sstream>

namespace homeguardian {

AnomalyResult AnomalyDetector::record(const std::string& key, double value,
                                      std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lk(m_);

    auto it = state_.find(key);
    if (it == state_.end()) {
        // New key: create state and bound the table.
        if (state_.size() >= cfg_.max_keys && !insertion_order_.empty()) {
            const std::string oldest = insertion_order_.front();
            insertion_order_.erase(insertion_order_.begin());
            state_.erase(oldest);
        }
        KeyState st;
        st.order = next_order_++;
        it = state_.emplace(key, std::move(st)).first;
        insertion_order_.push_back(key);
    }

    KeyState& st = it->second;
    st.last_seen = now;

    // Evaluate against the CURRENT baseline BEFORE adding the new sample, so a
    // sample is compared to the baseline established by prior observations.
    AnomalyResult result = evaluate(st, value);

    // Update the rolling window + running stats (bounded).
    if (st.window.size() >= cfg_.max_window) {
        double dropped = st.window.front();
        st.window.pop_front();
        st.sum -= dropped;
        st.sum_sq -= dropped * dropped;
    }
    st.window.push_back(value);
    st.sum += value;
    st.sum_sq += value * value;

    return result;
}

AnomalyResult AnomalyDetector::evaluate(KeyState& st, double value) const {
    AnomalyResult r;
    r.value = value;
    r.sample_count = st.window.size();

    const size_t n = st.window.size();
    if (n == 0) {
        r.reason = "first observation; baseline established, not evaluated";
        return r;  // not anomalous
    }

    const double mean = st.sum / static_cast<double>(n);
    const double var = (st.sum_sq / static_cast<double>(n)) - (mean * mean);
    const double stddev = var > 0.0 ? std::sqrt(var) : 0.0;
    r.mean = mean;
    r.stddev = stddev;

    // Warm-up: not enough samples to trust the baseline.
    if (n < cfg_.min_samples) {
        std::ostringstream os;
        os << "warming up (" << n << "/" << cfg_.min_samples
           << " samples); not evaluated";
        r.reason = os.str();
        return r;
    }

    if (stddev <= 0.0) {
        // Baseline is perfectly flat; any deviation is anomalous but z is
        // undefined. Treat an exact match as fine and a mismatch as high/low.
        if (value == mean) {
            r.reason = "flat baseline; value matches";
            return r;
        }
        r.anomalous = true;
        r.kind = value > mean ? AnomalyKind::high : AnomalyKind::low;
        std::ostringstream os;
        os << "flat baseline mean=" << mean << "; value=" << value
           << " deviates (stddev=0)";
        r.reason = os.str();
        return r;
    }

    const double z = (value - mean) / stddev;
    r.zscore = z;
    const double az = std::fabs(z);
    if (az > cfg_.zscore_threshold) {
        r.anomalous = true;
        r.kind = z > 0 ? AnomalyKind::high : AnomalyKind::low;
        std::ostringstream os;
        os << "value " << value << " is " << az << " sigma "
           << (z > 0 ? "above" : "below") << " baseline mean " << mean
           << " (stddev " << stddev << ", threshold " << cfg_.zscore_threshold
           << ", n=" << n << ")";
        r.reason = os.str();
    } else {
        std::ostringstream os;
        os << "within baseline (z=" << z << ", threshold " << cfg_.zscore_threshold
           << ", n=" << n << ")";
        r.reason = os.str();
    }
    return r;
}

AnomalyResult AnomalyDetector::check_missing(const std::string& key,
                                             std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lk(m_);
    AnomalyResult r;
    auto it = state_.find(key);
    if (it == state_.end()) {
        // Never seen: not "missing" in the anomaly sense (nothing to miss yet).
        r.reason = "key never observed; not evaluated for missing";
        return r;
    }
    auto gap = now - it->second.last_seen;
    if (gap >= cfg_.max_gap) {
        r.anomalous = true;
        r.kind = AnomalyKind::missing;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(gap).count();
        std::ostringstream os;
        os << "no observation for " << secs << "s (max_gap "
           << cfg_.max_gap.count() << "s)";
        r.reason = os.str();
    } else {
        r.reason = "recent observation present";
    }
    return r;
}

size_t AnomalyDetector::tracked_keys() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_.size();
}

size_t AnomalyDetector::sample_count(const std::string& key) const {
    std::lock_guard<std::mutex> lk(m_);
    auto it = state_.find(key);
    return it == state_.end() ? 0 : it->second.window.size();
}

void AnomalyDetector::reset() {
    std::lock_guard<std::mutex> lk(m_);
    state_.clear();
    insertion_order_.clear();
    next_order_ = 0;
}

void AnomalyDetector::evict_if_needed() {
    // Eviction is handled inline in record() when a new key is created; this
    // helper is retained for clarity and future use.
}

}  // namespace homeguardian

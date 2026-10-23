#pragma once

// AlertPolicy: a small, deterministic deduplication + cooldown layer applied to
// alerts produced by the existing Pipeline/ICorrelationRule engine.
//
// WHY: a TimeWindowCorrelation rule fires on every evaluation where the trigger
// condition holds (e.g. >=2 observations in the window). Feeding N observations
// one-by-one therefore yields up to N-1 alerts for one continuous condition —
// correct rule behavior, but noisy. AlertPolicy turns a repeated firing into a
// single notification per active condition, with an optional re-notification
// cooldown, WITHOUT suppressing unrelated alerts.
//
// POLICY (conservative, documented):
//  - Identity: an alert's dedup key is its rule_name (+ severity). Alerts with
//    different keys are tracked and delivered independently.
//  - First delivery: the first alert for a key is always delivered.
//  - Cooldown: subsequent alerts for the SAME key within cooldown_ms of the
//    last DELIVERED alert are suppressed (deduplicated) and counted.
//  - Re-arm / re-notify: after cooldown_ms elapse, the next alert for that key
//    is delivered again (re-notification). If the condition clears (a cycle
//    produces no alert for that key) the key is marked inactive and the next
//    alert is delivered immediately (fresh condition).
//  - Bounded state: retained per-key state and history are bounded (oldest
//    evicted); memory never grows without bound.
//  - Clock: an injectable time source (defaults to system clock) makes cooldown
//    and re-arm deterministic in tests. A clock that moves backward does not
//    crash; it simply cannot extend a cooldown (monotonic guard).
//
// This layer NEVER invents or drops a materially NEW alert (different key), and
// never hides the first occurrence of a condition. It is a notification policy,
// not a safety detector — a suppressed duplicate is still an active condition.

#include "core/Alert.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace homeguardian {

class AlertPolicy {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    // cooldown_ms: minimum interval between DELIVERED notifications for the same
    // key. 0 disables cooldown (every firing delivered). max_tracked_keys caps
    // retained dedup state (oldest evicted) to bound memory.
    AlertPolicy(std::chrono::milliseconds cooldown_ms = std::chrono::milliseconds(60000),
                size_t max_tracked_keys = 256,
                Clock clock = [] { return std::chrono::system_clock::now(); });

    // Decide whether an alert should be delivered now. `suppressed` is set true
    // when the alert is a deduplicated duplicate within cooldown. Returns true
    // when the alert should be delivered (first occurrence, re-notification
    // after cooldown, or a fresh condition after it cleared).
    bool admit(const Alert& alert, bool& suppressed);
    // Atomically advance cooldown only after the durable sink succeeds. Callback
    // must not re-enter this policy. Failed writes do not count as delivery.
    bool admit_durable(const Alert& alert, bool& suppressed, const std::function<bool()>& commit);

    // Mark that a cycle produced NO alert for a key (condition cleared). Re-arms
    // the key so its next alert is delivered immediately. Unknown keys are
    // ignored.
    void mark_condition_cleared(const std::string& rule_name);

    // Introspection.
    size_t suppressed_count() const;
    size_t delivered_count() const;
    size_t tracked_keys() const;

    // Reset all policy state (used on shutdown / deterministic re-runs).
    void reset();

private:
    struct KeyState {
        std::chrono::system_clock::time_point last_delivered{};
        bool active = false;         // condition currently firing
        uint64_t order = 0;          // insertion order for LRU-style eviction
    };

    std::string key_for(const Alert& alert) const;
    void evict_if_needed();

    std::chrono::milliseconds cooldown_ms_;
    size_t max_tracked_keys_;
    Clock clock_;

    mutable std::mutex m_;
    std::unordered_map<std::string, KeyState> state_;
    std::vector<std::string> insertion_order_;  // for bounded eviction
    size_t suppressed_count_ = 0;
    size_t delivered_count_ = 0;
    uint64_t next_order_ = 0;
};

}  // namespace homeguardian

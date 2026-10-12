#pragma once

// SimulatorRuntime: wires the simulator-first components into a single,
// deterministic, in-process runtime the running Application drives.
//
// This is the integration seam between the Application lifecycle and the
// existing simulator abstractions. It owns:
//  - an in-memory consent repository + ConsentGate (fail-closed),
//  - ONE shared AudioBroker (single simulated mic capture + arbitrated
//    speaker output),
//  - TWO independent LogicalDevice identities.
//
// SAFETY (simulator-only): it NEVER opens V4L2, ALSA, PipeWire, an Android
// camera, or a microphone. All media is the deterministic SimulatedSensorBackend
// (in-memory synthetic PCM/frames). There is no implicit consent: capture starts
// disabled and only delivers media once a synthetic grant is present AND
// config.media_capture_enabled is true. Every fan-out is authorized per consumer
// at the delivery boundary via the existing ConsentGate.
//
// The runtime is deterministic and memory-bounded, so the Application can
// exercise it in a fixed number of cycles and shut down cleanly.

#include "backend/simulator/AudioBroker.h"
#include "backend/simulator/LogicalDevice.h"
#include "core/ConsentGate.h"
#include "core/ConsentRecord.h"
#include "core/Event.h"
#include "core/PrivacyAuditLog.h"
#include "persistence/IConsentRepository.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace homeguardian {

// Minimal in-memory IConsentRepository for the simulator path. Kept local so the
// simulator runtime has no database dependency (mirrors the test fixtures, but
// lives in production code because the simulator mode is a first-class runtime).
class InMemoryConsentRepo : public IConsentRepository {
public:
    void save(const ConsentRecord& r) override {
        std::lock_guard<std::mutex> lk(m_);
        records_.push_back(r);
    }
    std::optional<ConsentRecord> find_by_id(const std::string&) override {
        return std::nullopt;
    }
    std::vector<ConsentRecord> find_by_subject(const std::string& s) override {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<ConsentRecord> out;
        for (auto& r : records_) if (r.get_subject_scope() == s) out.push_back(r);
        return out;
    }
    std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<ConsentRecord> out;
        for (auto& r : records_)
            if (r.get_subject_scope() == s && r.get_purpose() == p) out.push_back(r);
        return out;
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
private:
    std::mutex m_;
    std::vector<ConsentRecord> records_;
};

// Deterministic, simulator-only media runtime.
class SimulatorRuntime {
public:
    // media_capture_enabled mirrors config: the global kill switch. When false
    // (the default), the gate denies ALL acquisition regardless of grants.
    explicit SimulatorRuntime(bool media_capture_enabled = false,
                              size_t speaker_queue_capacity = 64);

    // Register the two canonical logical devices and their consent requests.
    // Distinct purposes per device so each has an independent grant.
    void register_default_devices();

    // Grant synthetic audio consent for a consumer id. This is the explicit
    // operator action that enables simulated delivery for that consumer only.
    void grant_consent(const std::string& consumer_id, const std::string& purpose);

    // Withdraw consent for a consumer id (blocks its subsequent delivery).
    void withdraw_consent(const std::string& consumer_id, const std::string& purpose);

    // Set the global capture kill switch at runtime (capture-disabled mode).
    void set_media_capture_enabled(bool enabled);

    // Install an event sink: each time tick() produces an AUTHORIZED synthetic
    // observation, it is passed to this callback (e.g. Application::process_event
    // -> Pipeline). Only authorized observations reach the sink; denied/withdrawn
    // consumers never produce events. Default sink discards (records only on the
    // logical device). Pass nullptr to clear.
    void set_event_sink(std::function<void(const Event&)> sink);

    // Optional privacy audit observer (roadmap H-03). When set, every per-consumer
    // delivery decision in tick() (authorized or denied) records a metadata-only
    // entry. Passive observer only: it never changes any decision. Non-owning;
    // may be null. Pass nullptr to detach.
    void set_privacy_audit(PrivacyAuditLog* log) { privacy_audit_ = log; }

    // Start the single shared capture session (idempotent-guarded).
    void start();

    // Run one media cycle: fan out one synthetic mic payload to authorized
    // consumers, and (for each authorized consumer that received audio) enqueue
    // a synthetic response and play it through the arbitrated speaker. Returns
    // the number of consumers that received media this cycle.
    int tick();

    // Deterministic, memory-bounded shutdown. Idempotent.
    void shutdown();

    // Re-arm after a shutdown so a subsequent scenario run can start() again
    // (demo control plane; simulator-only). Clears ONLY the shutdown latch when
    // the runtime is not currently started; it does not touch consent grants,
    // the shared repo, or any delivered event. This lets a single long-lived
    // native process run the scenario more than once (the legacy demo spawned a
    // fresh process per run, so this preserves that user-visible behavior).
    void reset_for_reuse();

    // --- Introspection (for status reporting + tests) ---
    size_t consumer_count() const;
    size_t delivered_count(const std::string& consumer_id) const;
    size_t last_fanout_blocked() const;
    bool speaker_busy() const;
    size_t speaker_queue_size() const;
    size_t speaker_played_count() const;
    size_t speaker_queue_dropped() const;
    bool capture_started() const;
    bool shut_down() const;

    // The two canonical logical device ids.
    static std::string enabled_device_id();
    static std::string assistant_device_id();

private:
    struct ConsumerRequest {
        std::string purpose;
        std::string data_category;
    };

    ConsumerRequest request_for(const std::string& consumer_id) const;

    InMemoryConsentRepo repo_;
    bool media_capture_enabled_;
    std::unique_ptr<ConsentGate> gate_;
    std::unique_ptr<AudioBroker> broker_;
    std::unique_ptr<LogicalDevice> enabled_device_;
    std::unique_ptr<LogicalDevice> assistant_device_;

    bool started_ = false;
    bool shut_down_ = false;
    uint64_t seq_ = 0;
    std::function<void(const Event&)> event_sink_;
    PrivacyAuditLog* privacy_audit_ = nullptr;  // non-owning; may be null
};

} // namespace homeguardian

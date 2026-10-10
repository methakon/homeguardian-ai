#include "AudioBroker.h"

#include <algorithm>

namespace homeguardian {

AudioBroker::AudioBroker(SimulatedSensorBackend::Config mic_cfg,
                         SimulatedSensorBackend::Config spk_cfg,
                         size_t output_queue_capacity)
    : mic_(DeviceKind::audio, "shared-sim-mic", std::move(mic_cfg)),
      speaker_(DeviceKind::audio, "shared-sim-speaker", std::move(spk_cfg)),
      output_queue_capacity_(output_queue_capacity > 0 ? output_queue_capacity : 1) {
    // The speaker is an OUTPUT path: initialize it so playback is available
    // regardless of capture consent. It is not a capture session and is never
    // gated by the capture-consent boundary.
    speaker_.initialize();
}

SimulatedSensorBackend& AudioBroker::mic() { return mic_; }
SimulatedSensorBackend& AudioBroker::speaker() { return speaker_; }

std::string AudioBroker::register_consumer(const std::string& consumer_id,
                                           ConsentGate& gate,
                                           AcquisitionRequest request,
                                           std::function<bool()> permission_ok) {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_) {
        throw std::runtime_error("audio broker has been shut down");
    }
    // Guard against duplicate registration of the same consumer id.
    for (const auto& c : consumers_) {
        if (c.id == consumer_id) {
            throw std::invalid_argument("consumer already registered: " + consumer_id);
        }
    }
    ConsumerEntry entry;
    entry.id = consumer_id;
    entry.gate = &gate;                         // non-owning
    entry.request = std::move(request);
    entry.permission_ok = std::move(permission_ok);
    entry.delivered_count = 0;
    consumers_.push_back(std::move(entry));
    return consumer_id;
}

size_t AudioBroker::consumer_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return consumers_.size();
}

void AudioBroker::start_capture() {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_) {
        throw std::runtime_error("audio broker has been shut down");
    }
    if (capture_started_) {
        throw std::runtime_error("shared capture already started");
    }
    // The broker owns the SINGLE shared capture session: initialize + start the
    // mic exactly once. Consumers do NOT drive this lifecycle; they only decide
    // (at the delivery boundary) whether they may receive each payload.
    mic_.initialize();
    mic_.start();
    capture_started_ = true;
}

std::vector<uint8_t> AudioBroker::capture_and_fanout() {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_) {
        return {};
    }
    if (!capture_started_) {
        throw std::runtime_error("shared capture not started");
    }

    // Determine which consumers are authorized RIGHT NOW, each via its own
    // consent gate + OS permission check. This is fail-closed and independent
    // per consumer: a denied consumer never stops the shared source.
    std::vector<bool> authorized(consumers_.size(), false);
    size_t any_authorized = 0;
    for (size_t i = 0; i < consumers_.size(); ++i) {
        const auto& c = consumers_[i];
        bool ok = c.permission_ok();
        if (ok) {
            GateDecision d = c.gate->check(c.request);
            ok = d.authorized;
        }
        authorized[i] = ok;
        if (ok) any_authorized++;
    }

    // If NO consumer is authorized, do not generate anything and fan out to no
    // one. This keeps the shared source quiet when nobody is allowed to listen.
    if (any_authorized == 0) {
        last_fanout_blocked_ = consumers_.size();
        return {};
    }

    // Generate ONE payload from the single shared capture source.
    std::vector<uint8_t> payload = mic_.generate_frame();

    // Fan the SAME bytes out to every authorized consumer. Blocked consumers
    // receive nothing.
    size_t blocked = 0;
    for (size_t i = 0; i < consumers_.size(); ++i) {
        if (authorized[i]) {
            consumers_[i].delivered_count++;
        } else {
            blocked++;
        }
    }
    last_fanout_blocked_ = blocked;
    return payload;
}

size_t AudioBroker::delivered_count(const std::string& consumer_id) const {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto& c : consumers_) {
        if (c.id == consumer_id) return c.delivered_count;
    }
    return 0;
}

size_t AudioBroker::last_fanout_blocked() const {
    std::lock_guard<std::mutex> lk(m_);
    return last_fanout_blocked_;
}

bool AudioBroker::enqueue_speaker(const std::string& consumer_id, std::vector<uint8_t> payload) {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_) {
        return false;
    }
    // Bounded queue: if full, reject the NEW job and count it as dropped. The
    // queue stays at capacity (memory never grows without bound); the caller is
    // told the response was NOT accepted so it can retry or discard.
    if (output_queue_.size() >= output_queue_capacity_) {
        output_queue_dropped_++;
        return false;
    }
    output_queue_.push_back(SpeakerJob{consumer_id, std::move(payload)});
    return true;
}

bool AudioBroker::play_next() {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_) {
        return false;
    }
    // Arbitration: never overlap playback. If a playback is in progress, refuse.
    if (speaker_busy_) {
        return false;
    }
    if (output_queue_.empty()) {
        return false;
    }
    // Play through the single simulated speaker. Playback is an OUTPUT path
    // (not capture), so there is no capture-consent gate here.
    //
    // Only remove the job from the queue AFTER a successful play: on a
    // playback error the job must remain queued so a later retry (after
    // recovery) can still deliver it.
    bool ok = speaker_.play(output_queue_.front().payload);
    if (!ok) {
        return false;   // job stays queued
    }
    output_queue_.erase(output_queue_.begin());
    speaker_busy_ = true;      // hold the speaker until finish_playback()
    speaker_played_count_++;
    return true;
}

void AudioBroker::finish_playback() {
    std::lock_guard<std::mutex> lk(m_);
    speaker_busy_ = false;
}

bool AudioBroker::speaker_busy() const {
    std::lock_guard<std::mutex> lk(m_);
    return speaker_busy_;
}

size_t AudioBroker::speaker_queue_size() const {
    std::lock_guard<std::mutex> lk(m_);
    return output_queue_.size();
}

size_t AudioBroker::speaker_queue_dropped() const {
    std::lock_guard<std::mutex> lk(m_);
    return output_queue_dropped_;
}

size_t AudioBroker::speaker_played_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return speaker_played_count_;
}

void AudioBroker::inject_speaker_error() {
    std::lock_guard<std::mutex> lk(m_);
    speaker_.inject_playback_error();
    speaker_busy_ = false;
}

void AudioBroker::recover_speaker() {
    std::lock_guard<std::mutex> lk(m_);
    // Deterministic recovery: close + re-init the simulated speaker.
    speaker_.close();
    speaker_.initialize();
    speaker_busy_ = false;
}

void AudioBroker::shutdown() {
    std::lock_guard<std::mutex> lk(m_);
    if (shutdown_) {
        return;  // idempotent
    }
    // Stop the single shared capture session (fail-closed: no further delivery
    // to any consumer) and release the mic.
    if (capture_started_) {
        mic_.stop();
        mic_.close();
    }
    // Clear the bounded output queue and release the speaker.
    output_queue_.clear();
    speaker_.stop();
    speaker_.close();
    speaker_busy_ = false;
    capture_started_ = false;
    shutdown_ = true;
}

} // namespace homeguardian

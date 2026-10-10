#pragma once

// Shared Ubuntu audio broker for the dual logical-device simulation.
//
// ONE microphone source, MANY consent-guarded consumers. The broker owns a
// single simulated microphone (one capture session — never competing/duplicate
// capture). Each registered consumer has its OWN ConsentGuardedDevice bound to
// the SAME shared mic, so every fan-out delivery is authorized independently at
// the delivery boundary. Revoking one consumer's consent blocks delivery to
// THAT consumer only; others are unaffected.
//
// ONE speaker output. Responses from all consumers are enued onto a single
// BOUNDED output queue and played through ONE speaker with ARBITRATION: only
// one playback runs at a time, so responses never overlap.
//
// SAFETY / HONESTY:
//  - The mic and speaker are the deterministic SimulatedSensorBackend (audio
//    kind). No physical microphone or speaker is opened; no AWS, no Alexa OS.
//  - Speaker playback is an OUTPUT path and is NOT gated by the capture-consent
//    boundary (it is not acquisition). Capture fan-out IS gated per consumer.
//  - Failures are explicit (returned/throwable), never silently dropped.
//  - Shutdown is deterministic: stop capture, clear the bounded queue, release
//    the speaker, and reset consumers to a released state.

#include "backend/simulator/SimulatedSensorBackend.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace homeguardian {

// A queued speaker response (tagged with its originating consumer).
struct SpeakerJob {
    std::string consumer_id;
    std::vector<uint8_t> payload;
};

class AudioBroker {
public:
    // Constructs the broker with ONE shared simulated mic and ONE simulated
    // speaker. `mic_cfg`/`spk_cfg` configure the deterministic backends;
    // `output_queue_capacity` bounds the speaker queue.
    AudioBroker(SimulatedSensorBackend::Config mic_cfg,
                SimulatedSensorBackend::Config spk_cfg,
                size_t output_queue_capacity = 64);

    // --- Shared microphone source (single capture session) ---
    SimulatedSensorBackend& mic();
    SimulatedSensorBackend& speaker();

    // Register a consumer bound to the shared mic via its own consent gate +
    // request. Multiple consumers may register; each gets an independent guard
    // over the same capture source. `permission_ok` models OS permission per
    // consumer. Returns the consumer id.
    std::string register_consumer(const std::string& consumer_id,
                                  ConsentGate& gate,
                                  AcquisitionRequest request,
                                  std::function<bool()> permission_ok = [] { return true; });

    size_t consumer_count() const;

    // Start the single shared capture session and initialize all consumers'
    // guards. Each consumer start is gated independently; a consumer whose
    // consent is denied does not start, but does not block the others.
    void start_capture();

    // Produce ONE microphone payload and fan it out to every consumer whose
    // consent authorizes delivery AT THIS BOUNDARY. Consumers whose consent is
    // denied/withdrawn/revoked receive nothing (fail-closed) and are recorded
    // as blocked. Returns the payload actually produced by the shared source
    // (empty if capture is not authorized by ANY consumer, in which case no
    // fan-out occurs and nothing is generated).
    //
    // The shared source generates a payload only when at least one consumer is
    // authorized; the SAME bytes are offered to each authorized consumer.
    std::vector<uint8_t> capture_and_fanout();

    // Per-consumer authorized delivery count.
    size_t delivered_count(const std::string& consumer_id) const;

    // Number of consumers that were blocked at the most recent fan-out.
    size_t last_fanout_blocked() const;

    // --- Speaker output (bounded queue + arbitration) ---
    // Enqueue a response for the single speaker. Bounded: if the queue is full,
    // the NEW response is rejected (returns false) and counted as dropped, so
    // memory never grows without bound. Only one playback runs at a time.
    bool enqueue_speaker(const std::string& consumer_id, std::vector<uint8_t> payload);

    // Play the next queued job through the single speaker. Arbitration: if a
    // playback is already in progress, this is a no-op returning false (no
    // overlapping responses). Returns true when a job was played.
    bool play_next();

    // Finish the in-progress playback (models playback completing). Only then
    // can the next job play.
    void finish_playback();

    bool speaker_busy() const;
    size_t speaker_queue_size() const;
    size_t speaker_queue_dropped() const;
    size_t speaker_played_count() const;

    // --- Error injection / recovery (tests) ---
    void inject_speaker_error();
    void recover_speaker();

    // --- Deterministic shutdown ---
    // Stop capture, clear the bounded queue, release the speaker, and mark all
    // consumers released. Idempotent.
    void shutdown();

private:
    struct ConsumerEntry {
        std::string id;
        ConsentGate* gate = nullptr;
        AcquisitionRequest request;
        std::function<bool()> permission_ok = [] { return true; };
        size_t delivered_count = 0;
    };

    SimulatedSensorBackend mic_;
    SimulatedSensorBackend speaker_;
    size_t output_queue_capacity_;

    mutable std::mutex m_;
    std::vector<ConsumerEntry> consumers_;
    bool capture_started_ = false;
    size_t last_fanout_blocked_ = 0;

    // Speaker state.
    std::vector<SpeakerJob> output_queue_;
    size_t output_queue_dropped_ = 0;
    bool speaker_busy_ = false;
    size_t speaker_played_count_ = 0;
    bool shutdown_ = false;
};

} // namespace homeguardian

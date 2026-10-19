#include "action/AlexaIntegrationBoundary.h"

namespace homeguardian::action {

bool AlexaProactiveSecurityEvent::is_privacy_compliant() const {
    // Structural invariant: must contain zero raw audio samples, video buffers,
    // or unredacted sensor media frames.
    const std::vector<std::string> prohibited_keys = {
        "audio_samples", "video_frames", "raw_media", "camera_stream",
        "raw_audio", "video_stream", "frame_buffer", "microphone_stream",
        "pcm_data", "raw_stream", "microphone_pcm"
    };
    for (const auto& key : prohibited_keys) {
        if (metadata.contains(key)) {
            return false;
        }
    }
    return true;
}

nlohmann::json AlexaProactiveSecurityEvent::to_proactive_event_json() const {
    const int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()).count();

    return {
        {"header", {
            {"namespace", "Alexa.SecurityEvent"},
            {"name", "SecurityEventTriggered"},
            {"messageId", event_correlation_id},
            {"payloadVersion", "3"}
        }},
        {"endpoint", {
            {"endpointId", endpoint_id}
        }},
        {"payload", {
            {"detectionType", detection_type},
            {"threatLevel", threat_level},
            {"timestamp", ts_ms},
            {"metadata", metadata}
        }}
    };
}

MockAlexaProactiveEventsAdapter::MockAlexaProactiveEventsAdapter(bool configured)
    : configured_(configured), connected_(configured) {}

ActionResult MockAlexaProactiveEventsAdapter::dispatch_event(
    const ActionRequest& request, const AlexaProactiveSecurityEvent& event) {
    ActionResult res;
    res.request_id = request.request_id;
    res.idempotency_key = request.idempotency_key;
    res.action_type = request.action_type;
    res.executed_at = std::chrono::system_clock::now();

    // 1. Fail-closed if adapter is unconfigured or disconnected
    if (!configured_ || !connected_) {
        res.state = ExecutionState::Failed;
        res.success = false;
        res.failure_reason = "Alexa integration is not configured / unconfigured (requires official LWA / Amazon Developer credentials)";
        return res;
    }

    // 2. Strict Privacy Guard validation
    if (!event.is_privacy_compliant()) {
        res.state = ExecutionState::Failed;
        res.success = false;
        res.failure_reason = "Privacy guard violation: Attempted to include prohibited raw media in outbound event";
        return res;
    }

    dispatched_events_.push_back(event);
    res.state = ExecutionState::Completed;
    res.success = true;
    res.output = {
        {"status", "simulated_proactive_event_dispatched"},
        {"proactive_payload", event.to_proactive_event_json()}
    };
    return res;
}

bool AlexaPrivacyGuard::validate_no_raw_media_exfiltration(const nlohmann::json& payload) {
    if (payload.contains("audio_samples") ||
        payload.contains("video_frames") ||
        payload.contains("raw_stream") ||
        payload.contains("microphone_pcm")) {
        return false;
    }
    return true;
}

bool AlexaPrivacyGuard::validate_fail_closed_unconfigured(const IAlexaIntegrationAdapter& adapter) {
    if (!adapter.is_configured()) {
        return true; // Correct: unconfigured adapter must fail closed
    }
    return adapter.is_connected();
}

} // namespace homeguardian::action

#pragma once

#include "action/ActionContract.h"
#include <chrono>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace homeguardian::action {

// Distinct architectural tiers for Alexa & Alexa+ interactions
enum class AlexaIntegrationTier {
    Tier1_SimulatedLocalClient,      // Loopback web demonstration client (non-Amazon, in-repo)
    Tier2_NativeMcpServer,           // Self-hosted native C++ Model Context Protocol 2025-11-25 server
    Tier3_OfficialAmazonIntegration  // Official Alexa Smart Home / Proactive Events API (unconfigured by default)
};

inline std::string alexa_tier_to_string(AlexaIntegrationTier tier) {
    switch (tier) {
        case AlexaIntegrationTier::Tier1_SimulatedLocalClient:
            return "Tier 1: Simulated Local Demonstration Client (In-Repo Loopback)";
        case AlexaIntegrationTier::Tier2_NativeMcpServer:
            return "Tier 2: Native C++ Model Context Protocol (MCP 2025-11-25) Server";
        case AlexaIntegrationTier::Tier3_OfficialAmazonIntegration:
            return "Tier 3: Official Amazon Smart Home / Proactive Events Adapter";
    }
    return "Unknown Tier";
}

// Data contract for potential official Alexa Proactive Events (Metadata Only)
struct AlexaProactiveSecurityEvent {
    std::string endpoint_id;             // Scoped endpoint identifier (e.g. "hg-sensor-01")
    std::string detection_type;          // e.g. "ANOMALY_DETECTED", "MOTION_ACTIVITY"
    std::string threat_level;            // "LOW", "MEDIUM", "HIGH"
    std::string event_correlation_id;    // Correlates back to Alert ID
    std::chrono::system_clock::time_point timestamp;
    nlohmann::json metadata;

    // Invariant: Verifies zero raw audio or video payload leaks
    bool is_privacy_compliant() const;

    nlohmann::json to_proactive_event_json() const;
};

// Abstract interface for official or simulated Alexa adapters
class IAlexaIntegrationAdapter {
public:
    virtual ~IAlexaIntegrationAdapter() = default;

    virtual AlexaIntegrationTier get_tier() const = 0;
    virtual std::string get_adapter_name() const = 0;
    virtual bool is_configured() const = 0;
    virtual bool is_connected() const = 0;

    // Dispatches proactive alert event (metadata only)
    virtual ActionResult dispatch_event(const ActionRequest& request, const AlexaProactiveSecurityEvent& event) = 0;
};

// Mock / Local Adapter for official Proactive Events testing without cloud access
class MockAlexaProactiveEventsAdapter : public IAlexaIntegrationAdapter {
public:
    MockAlexaProactiveEventsAdapter(bool configured = false);
    ~MockAlexaProactiveEventsAdapter() override = default;

    AlexaIntegrationTier get_tier() const override { return AlexaIntegrationTier::Tier3_OfficialAmazonIntegration; }
    std::string get_adapter_name() const override { return "mock_alexa_proactive_events_adapter"; }
    bool is_configured() const override { return configured_; }
    bool is_connected() const override { return connected_; }

    void set_configured(bool conf) { configured_ = conf; }
    void set_connected(bool conn) { connected_ = conn; }

    ActionResult dispatch_event(const ActionRequest& request, const AlexaProactiveSecurityEvent& event) override;

    size_t get_dispatched_count() const { return dispatched_events_.size(); }
    std::vector<AlexaProactiveSecurityEvent> get_dispatched_events() const { return dispatched_events_; }
    void clear() { dispatched_events_.clear(); }

private:
    bool configured_ = false;
    bool connected_ = false;
    std::vector<AlexaProactiveSecurityEvent> dispatched_events_;
};

// Structural validator enforcing HomeGuardian's privacy boundary on Alexa interactions
struct AlexaPrivacyGuard {
    static bool validate_no_raw_media_exfiltration(const nlohmann::json& payload);
    static bool validate_fail_closed_unconfigured(const IAlexaIntegrationAdapter& adapter);
};

} // namespace homeguardian::action

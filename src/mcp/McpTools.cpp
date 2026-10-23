#include "mcp/McpTools.h"

#include <variant>

#include <nlohmann/json.hpp>

namespace homeguardian::mcp {
namespace {

using nlohmann::json;

// One generic failure message for every refusal. The contract's own rule is that errors
// carry no internal paths, stack details or diagnostics; a client must not be able to
// tell "expired token" from "not permitted for this operation" from "application
// unavailable". The reason is logged server-side instead.
constexpr const char* kGenericFailure = "the request could not be authorized";

// The analysis channel (see docs/MCP_SERVER.md): the server log records WHICH category
// failed, never the credential and never a client-visible detail.
struct ContractOutcome {
    bool ok = false;
    AlexaError error = AlexaError::none;
    AlexaResponse response;
};

ContractOutcome run_contract(const char* operation,
                             const std::string& credential,
                             const IAlexaCredentialVerifier& verifier,
                             const IAlexaStatusProvider& provider) {
    ContractOutcome out;
    json request{{"version", ALEXA_CONTRACT_VERSION},
                 {"operation", operation},
                 {"credential", credential}};

    auto parsed = AlexaContract::parse(request.dump());
    if (std::holds_alternative<AlexaError>(parsed)) {
        out.error = std::get<AlexaError>(parsed);
        return out;
    }
    out.response = AlexaContract::handle(std::get<AlexaRequest>(parsed), verifier, provider);
    out.ok = out.response.ok;
    out.error = out.response.error;
    return out;
}

ToolOutcome status_tool(const std::string& /*args*/,
                        const std::string& credential,
                        const IAlexaCredentialVerifier& verifier,
                        const IAlexaStatusProvider& provider) {
    const ContractOutcome r = run_contract("status", credential, verifier, provider);
    if (!r.ok) return ToolOutcome{false, kGenericFailure};
    json out{{"state", r.response.state},
             {"capture_enabled", r.response.capture_enabled},
             {"read_only", true}};
    return ToolOutcome{true, out.dump()};
}

ToolOutcome alert_summary_tool(const std::string& /*args*/,
                               const std::string& credential,
                               const IAlexaCredentialVerifier& verifier,
                               const IAlexaStatusProvider& provider) {
    const ContractOutcome r = run_contract("alert_summary", credential, verifier, provider);
    if (!r.ok) return ToolOutcome{false, kGenericFailure};
    json out{{"alert_count", r.response.alert_count},
             {"highest_severity", r.response.highest_severity},
             {"alert_ids", r.response.alert_ids},
             {"read_only", true}};
    return ToolOutcome{true, out.dump()};
}

}  // namespace

ToolOutcome devices_tool(const std::string& /*args*/,
                         const std::string& credential,
                         const IAlexaCredentialVerifier& verifier,
                         const IAlexaStatusProvider& provider) {
    const ContractOutcome r = run_contract("devices", credential, verifier, provider);
    if (!r.ok) return ToolOutcome{false, kGenericFailure};
    json out{{"device_count", r.response.device_count},
             {"device_labels", r.response.device_labels},
             {"visibility", r.response.device_visibility},
             {"read_only", true}};
    return ToolOutcome{true, out.dump()};
}

ToolRegistry make_homeguardian_tools(const IAlexaCredentialVerifier& verifier,
                                     const IAlexaStatusProvider& provider) {
    ToolRegistry registry;

    registry.add(Tool{
        kToolStatus,
        "Read-only lifecycle status of the HomeGuardian application: lifecycle state and "
        "whether synthetic capture is enabled. Takes no arguments. Cannot change state.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        [&verifier, &provider](const std::string& args, const std::string& cred) {
            return status_tool(args, cred, verifier, provider);
        }});


    registry.add(Tool{
        kToolAlertSummary,
        "Read-only aggregate alert summary: total alert count, highest severity, and a "
        "capped list of non-sensitive alert ids. Takes no arguments. Never returns raw "
        "media, personal identifiers or alert payloads.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        [&verifier, &provider](const std::string& args, const std::string& cred) {
            return alert_summary_tool(args, cred, verifier, provider);
        }});
    registry.add(Tool{
        kToolDevices,
        "Read-only device visibility: how many devices the owner has approved as visible to an "
        "authenticated read-only viewer, and their opaque labels. Takes no arguments. Never "
        "returns device names, serials, network addresses, media, telemetry or per-device "
        "history, and cannot change state. Visibility is default-deny: labels are empty until "
        "the owner approves them, so a zero count means nothing is approved, not that no "
        "devices exist.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        [&verifier, &provider](const std::string& args, const std::string& cred) {
            return devices_tool(args, cred, verifier, provider);
        }});

    return registry;
}

}  // namespace homeguardian::mcp

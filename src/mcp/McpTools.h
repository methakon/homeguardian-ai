#pragma once

// The two read-only MCP tools, built on the EXISTING AlexaContract.
//
// Why delegate instead of reading the application directly: the contract is already the
// single place where a read request is parsed, authenticated, authorized and shaped into
// an allowlisted response, and it already fails closed. Re-implementing any of that here
// would create a second, divergent authorization path. So a tools/call is turned into the
// same kind of request the NDJSON bridge sends, with the caller's bearer token as the
// credential, and the contract decides.
//
// Consequence worth stating plainly: these tools CANNOT enable capture, grant consent,
// change configuration or activate a sensor, because no such operation exists in the
// contract and the request builder below only names read operations.

#include <string>

#include "core/AlexaContract.h"
#include "mcp/McpProtocol.h"

namespace homeguardian::mcp {

inline constexpr const char* kToolStatus = "get_homeguardian_status";
inline constexpr const char* kToolAlertSummary = "get_alert_summary";

// Build the registry. Both dependencies are injected and must outlive the registry — the
// same pattern the bridge uses, so tests can inject deterministic fakes.
ToolRegistry make_homeguardian_tools(const IAlexaCredentialVerifier& verifier,
                                     const IAlexaStatusProvider& provider);

}  // namespace homeguardian::mcp

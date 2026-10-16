#pragma once

// Streamable HTTP transport for the MCP server.
//
// Implemented behaviour (each item is also asserted by tests/mcp and scripts/verify_mcp.sh):
//   POST /mcp   JSON-RPC request/notification; reply as application/json, or as an SSE
//               stream when the client accepts only text/event-stream; 202 with no body
//               when the payload carried no request.
//   GET  /mcp   405 Method Not Allowed with an Allow header: this server offers no
//               server-initiated stream, which the specification permits.
//   DELETE /mcp session termination.
//   Sessions    minted on initialize (Mcp-Session-Id), required afterwards, idle-expired,
//               bounded in number.
//   Origin      if an Origin header is present it must match the allowlist exactly (DNS
//               rebinding defence); otherwise 403.
//   Auth        bearer token verified by the injected IAlexaCredentialVerifier; 401 with
//               WWW-Authenticate on failure. Never trusts a client-supplied identity.
//   Limits      bounded header size, bounded body size, read/write timeouts, bounded
//               worker count and a bounded in-flight cap (503 + Retry-After when full).
//   Shutdown    serve() returns promptly on the stop flag; the listener is closed and all
//               workers are joined.
//
// Binding: loopback only unless allow_remote is set explicitly. That default is the
// security boundary — a non-loopback bind with a weak token would be an unauthenticated
// internet-facing service.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/AlexaContract.h"
#include "mcp/McpProtocol.h"

namespace homeguardian::mcp {

struct McpHttpConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 8790;
    bool allow_remote = false;
    std::vector<std::string> allowed_origins;  // exact match; empty => Origin always refused
    bool require_auth = true;
    size_t max_body_bytes = kMaxJsonBytes;
    size_t max_header_bytes = 16 * 1024;
    int read_timeout_ms = 3000;
    int write_timeout_ms = 3000;
    size_t workers = 4;
    size_t max_inflight = 16;
    size_t max_sessions = 64;
    int session_idle_seconds = 1800;
};

class McpHttpServer {
public:
    McpHttpServer(McpHttpConfig cfg,
                  const ToolRegistry& tools,
                  const IAlexaCredentialVerifier& verifier);
    ~McpHttpServer();

    McpHttpServer(const McpHttpServer&) = delete;
    McpHttpServer& operator=(const McpHttpServer&) = delete;

    // Bind + listen. Returns false and fills `error` on failure (including a refused
    // non-loopback bind).
    bool start(std::string& error);

    // Accept loop; returns when `stop` is set. Blocks the calling thread.
    void serve(std::atomic<bool>& stop);

    uint16_t port() const { return bound_port_; }

    // Replace the Origin allowlist. MUST be called before serve(): the transport reads the
    // list from its worker threads, and setting it first makes that ordering explicit.
    void set_allowed_origins(const std::vector<std::string>& origins);

    // Counters for the demo/log (non-sensitive).
    struct Stats {
        uint64_t requests = 0;
        uint64_t rejected_origin = 0;
        uint64_t rejected_auth = 0;
        uint64_t rejected_oversize = 0;
        uint64_t rejected_too_many = 0;
        uint64_t errors = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint16_t bound_port_ = 0;
};

// True when `addr` is a loopback literal (127.0.0.0/8 or ::1). Exposed for testing.
bool is_loopback_literal(const std::string& addr);

// Reads cryptographically secure random bytes from the OS (/dev/urandom).
// Returns true on success, false if the OS entropy source fails. Exposed for testing.
bool read_os_entropy(void* buf, size_t len);

// Generates an unpredictable 256-bit (64-char hex) session ID using OS CSPRNG.
// Throws std::runtime_error("csprng_entropy_unavailable") if secure entropy is unavailable.
std::string mint_csprng_session_id();

}  // namespace homeguardian::mcp

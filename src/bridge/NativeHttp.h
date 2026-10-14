#pragma once
// HomeGuardian native demo HTTP transport (Linux/POSIX).
// Separate, opt-in component: it does not alter the existing C++ bridge,
// consent gate, dashboard, or the existing NDJSON tests.
// Loopback ONLY; GET-only; one request at a time; no dependencies beyond libc++.
// This limited HTTP/1.1 subset is NOT a general-purpose/public HTTP server and
// MUST NOT be exposed beyond 127.0.0.1 (see docs/SECURITY_READINESS_V8.md).
// A reviewed, production-grade HTTP parser + TLS + real authentication are
// required before any non-local use; see the H-04 review.
#ifndef _WIN32
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <string>
#include <string_view>
#include <iostream>

namespace hg_native_http {

// Request/response bounds (bytes). A request larger than the header cap is
// rejected 413; a request line this long is rejected 414-style as malformed.
inline constexpr size_t kMaxHeaderBytes = 8192;
inline constexpr size_t kMaxPathBytes = 128;
inline constexpr size_t kRecvChunk = 2048;

// A parsed, validated routing result handed to the handler.
struct HandlerRequest {
    std::string_view op;        // "status" | "scenario" | "alerts" | "scenarios"
    std::string_view scenario;  // scenario token; "" => caller's default
    unsigned cycles = 5;        // simulator ticks (valid only when cycles_set)
    bool cycles_set = false;    // true when the caller explicitly set cycles
};

// A handler receives a validated request and MUST return a JSON body. It also
// reports the HTTP status for the outcome it decided (200 for success; 400 for a
// well-formed request naming an unsupported value such as an unknown scenario).
// The transport never invents a business status and the handler never parses
// HTTP. Operations:
//   "status"    -- read-only status/summary
//   "scenario"  -- run the selected synthetic scenario (uses `scenario`/`cycles`)
//   "alerts"    -- read-only recent alert detail
//   "scenarios" -- read-only scenario catalogue
// The native integration calls the existing C++ backend (Application/
// SimulatorRuntime/SQLite); it must never synthesize data.
using Handler = std::function<std::string(const HandlerRequest&, int& status_out)>;

// Stop flag for clean shutdown. A signal handler sets this (async-signal-safe);
// the accept loop notices it within one listener-timeout tick.
inline std::atomic<bool> g_stop{false};

inline void request_shutdown() {
    g_stop.store(true);
}

inline bool is_loopback_authority(std::string_view host) {
    if (host.empty()) return false;
    // Strip an optional port (host:port) — only digits allowed after ':'.
    const auto colon = host.find(':');
    std::string_view h = colon == std::string_view::npos ? host : host.substr(0, colon);
    if (colon != std::string_view::npos) {
        const std::string_view port = host.substr(colon + 1);
        if (port.empty() || port.size() > 5) return false;
        for (char c : port) if (c < '0' || c > '9') return false;
    }
    return h == "127.0.0.1" || h == "localhost";
}

inline bool is_loopback_origin(std::string_view origin) {
    constexpr std::string_view prefix = "http://";
    if (origin.size() <= prefix.size() || origin.substr(0, prefix.size()) != prefix)
        return false;  // reject "null", https from elsewhere, other schemes
    return is_loopback_authority(origin.substr(prefix.size()));
}

inline std::string response(unsigned status, std::string_view mime, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request"
        : status == 403 ? "Forbidden" : status == 404 ? "Not Found"
        : status == 405 ? "Method Not Allowed" : status == 413 ? "Payload Too Large"
        : "Service Unavailable";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason
        + "\r\nContent-Type: " + std::string(mime)
        + "\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
        + "\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer"
        + "\r\nCross-Origin-Resource-Policy: same-origin";
    // The dashboard is fully self-contained (no external refs); a restrictive
    // CSP is defence-in-depth against an XSS that would otherwise run remote JS.
    if (mime.rfind("text/html", 0) == 0) {
        head += "\r\nContent-Security-Policy: default-src 'self'; "
                "script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
                "img-src 'self' data:; connect-src 'self'; base-uri 'none'; "
                "form-action 'none'; frame-ancestors 'none'";
    }
    head += "\r\nConnection: close\r\n\r\n";
    return head + body;
}

inline std::string json_error(unsigned status, std::string_view error) {
    return response(status, "application/json",
        "{\"ok\":false,\"error\":\"" + std::string(error) + "\"}");
}

inline std::string dispatch(std::string_view request, std::string_view html,
                            const Handler& handler) {
    const auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string_view::npos || request.size() != header_end + 4)
        return json_error(400, "malformed");
    const auto first_end = request.find("\r\n");
    if (first_end == std::string_view::npos || first_end > header_end)
        return json_error(400, "malformed");
    const auto line = request.substr(0, first_end);
    const auto p1 = line.find(' ');
    const auto p2 = p1 == std::string_view::npos ? p1 : line.find(' ', p1 + 1);
    if (p1 == std::string_view::npos || p2 == std::string_view::npos ||
        line.substr(p2 + 1) != "HTTP/1.1")
        return json_error(400, "malformed");
    if (line.substr(0, p1) != "GET")
        return json_error(405, "method_not_allowed");
    const auto path = line.substr(p1 + 1, p2 - p1 - 1);
    if (path.empty() || path.size() > kMaxPathBytes || path.front() != '/' ||
        path.find('%') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos ||
        path.find('#') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos)
        return json_error(400, "malformed");
    // Header scan. Reject: bodies, framing ambiguity, injected lines, all
    // Authorization headers (this interface is unauthenticated by design), a
    // missing/duplicate/non-loopback Host (DNS-rebinding defence), and any
    // non-loopback Origin (cross-site request defence for the scenario route).
    bool host_seen = false;
    bool origin_ok = true;
    size_t at = first_end + 2;
    while (at < header_end) {
        const auto next = request.find("\r\n", at);
        if (next == std::string_view::npos || next > header_end)
            return json_error(400, "malformed");
        const auto header = request.substr(at, next - at);
        const auto colon = header.find(':');
        if (colon == std::string_view::npos || colon == 0 ||
            header.front() == ' ' || header.front() == '\t')
            return json_error(400, "malformed");
        std::string key(header.substr(0, colon));
        for (char& ch : key)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        // Trim optional whitespace after the colon.
        size_t vstart = colon + 1;
        while (vstart < header.size() && (header[vstart] == ' ' || header[vstart] == '\t'))
            ++vstart;
        const auto value = header.substr(vstart);
        if (key == "host") {
            if (host_seen) return json_error(400, "malformed");
            host_seen = true;
            if (!is_loopback_authority(value)) return json_error(403, "forbidden_host");
        } else if (key == "origin") {
            if (!is_loopback_origin(value)) origin_ok = false;
        }
        if (key == "content-length" || key == "transfer-encoding" ||
            key == "authorization" || key == "expect")
            return json_error(400, "malformed");
        at = next + 2;
    }
    if (!host_seen) return json_error(400, "malformed");
    if (!origin_ok) return json_error(403, "forbidden_origin");
    if (path == "/" || path == "/index.html")
        return response(200, "text/html; charset=utf-8", std::string(html));
    if (path == "/api/health")
        return response(200, "application/json",
                        R"({"ok":true,"status":"up","simulated":true})");
    // The handler decides the business outcome, including a non-200 status when
    // it rejects a well-formed request (e.g. an unknown scenario id). The
    // transport owns framing, header and query validation only.
    auto invoke = [&handler](const HandlerRequest& r) {
        int status = 200;
        std::string body = handler(r, status);
        return response(static_cast<unsigned>(status), "application/json", body);
    };
    if (path == "/api/status")
        return invoke({"status", "", 5, false});
    if (path == "/api/alerts")
        return invoke({"alerts", "", 5, false});
    if (path == "/api/scenarios")
        return invoke({"scenarios", "", 5, false});
    // Sensor/integration status: read-only, no state change. Reports what is
    // ACTUALLY connected (currently: nothing) rather than implying hardware.
    if (path == "/api/integrations")
        return invoke({"integrations", "", 5, false});
    if (path == "/api/run-scenario" ||
        path.substr(0, 18) == "/api/run-scenario?") {
        // State-advancing route: bounded, allowlisted query only. Accepts at
        // most one `cycles=<digits>` and one `scenario=<token>`; any other key,
        // duplicate, empty value, or malformed token is rejected 400.
        HandlerRequest req;
        req.op = "scenario";
        const auto qmark = path.find('?');
        if (qmark != std::string_view::npos) {
            std::string_view q = path.substr(qmark + 1);
            if (q.empty()) return json_error(400, "invalid_query");
            bool scenario_seen = false;
            while (!q.empty()) {
                const auto amp = q.find('&');
                const std::string_view pair =
                    amp == std::string_view::npos ? q : q.substr(0, amp);
                q = amp == std::string_view::npos ? std::string_view{}
                                                  : q.substr(amp + 1);
                if (pair.empty()) return json_error(400, "invalid_query");
                const auto eq = pair.find('=');
                if (eq == std::string_view::npos)
                    return json_error(400, "invalid_query");
                const std::string_view key = pair.substr(0, eq);
                const std::string_view val = pair.substr(eq + 1);
                if (key == "cycles") {
                    if (req.cycles_set || val.empty() || val.size() > 3)
                        return json_error(400, "invalid_cycles");
                    req.cycles_set = true;
                    req.cycles = 0;
                    for (char ch : val) {
                        if (ch < '0' || ch > '9')
                            return json_error(400, "invalid_cycles");
                        req.cycles = req.cycles * 10 +
                                     static_cast<unsigned>(ch - '0');
                    }
                    if (req.cycles > 100)
                        return json_error(400, "invalid_cycles");
                } else if (key == "scenario") {
                    if (scenario_seen || val.empty() || val.size() > 32)
                        return json_error(400, "invalid_scenario");
                    scenario_seen = true;
                    for (char ch : val) {
                        const bool ok = (ch >= 'a' && ch <= 'z') ||
                                        (ch >= '0' && ch <= '9') || ch == '-';
                        if (!ok) return json_error(400, "invalid_scenario");
                    }
                    req.scenario = val;
                } else {
                    return json_error(400, "invalid_query");
                }
            }
        }
        return invoke(req);
    }
    return json_error(404, "not_found");
}

inline void send_all(int fd, const std::string& bytes) {
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + written,
                                 bytes.size() - written, MSG_NOSIGNAL);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
}

// Synchronous, bounded, loopback-only server. Handler executes on the caller
// thread. Intended solely for a controlled local simulator demo.
// Port=0 chooses an ephemeral port and announces "DEMO_READY <port>".
// Returns only after a bind/listen/accept failure or request_shutdown().
inline int serve(uint16_t port, std::string_view html, const Handler& handler) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 2;
    const int reuse = 1;
    (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 8) != 0) {
        ::close(listener);
        return 3;
    }
    g_stop.store(false);
    // A 1s listener timeout makes accept() poll-wake so a requested shutdown
    // is honored promptly regardless of SA_RESTART semantics.
    const timeval ltimeout{1, 0};
    (void)::setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &ltimeout, sizeof(ltimeout));
    socklen_t addr_size = sizeof(addr);
    (void)::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_size);
    std::cout << "DEMO_READY " << ntohs(addr.sin_port) << std::endl;
    for (;;) {
        if (g_stop.load()) break;
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (g_stop.load()) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        const timeval timeout{3, 0};
        (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string input;
        char buffer[kRecvChunk];
        bool too_large = false;
        while (input.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            if (input.size() + static_cast<size_t>(n) > kMaxHeaderBytes) {
                too_large = true;
                break;
            }
            input.append(buffer, static_cast<size_t>(n));
        }
        const auto output = too_large ? json_error(413, "too_large")
                                      : dispatch(input, html, handler);
        send_all(client, output);
        ::close(client);
    }
    ::close(listener);
    return 4;
}
}  // namespace hg_native_http
#endif  // !_WIN32

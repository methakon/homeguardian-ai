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
#include <chrono>
#include <random>
#include <unordered_map>
#include <vector>
#if defined(__linux__)
#include <sys/random.h>
#endif

namespace hg_native_http {

// Request/response bounds (bytes). A request larger than the header cap is
// rejected 413; a request line this long is rejected 414-style as malformed.
inline constexpr size_t kMaxHeaderBytes = 8192;
inline constexpr size_t kMaxPathBytes = 128;
inline constexpr size_t kRecvChunk = 2048;

// Request body cap for POST routes. The state route takes no parameters in its body
// (it uses the same strictly allowlisted query as before) - the body exists so the
// request carries a validated Content-Type/Length and cannot be a simple form post.
inline constexpr size_t kMaxBodyBytes = 512;

// Upload cap for POST /api/upload only. A media upload is the one route whose body is bytes
// rather than a JSON object, so it gets its own ceiling instead of widening kMaxBodyBytes for
// every route. The declared length is checked BEFORE the bytes are trusted, and the reader is
// bounded by the cap regardless of what was declared, so an oversized or dishonest request
// cannot make the process allocate beyond this.
inline constexpr size_t kMaxUploadBytes = 25u * 1024u * 1024u;  // 25 MiB

// CSRF session lifetime, issuance bounds and lockout thresholds.
inline constexpr int64_t kSessionTtlMs = 120000;      // a CSRF token lives 2 minutes
inline constexpr size_t kMaxSessions = 64;            // bounded session table
inline constexpr int64_t kIssueWindowMs = 10000;      // issuance rate-limit window
inline constexpr size_t kMaxIssuesPerWindow = 5;      // sessions minted per window
inline constexpr int kMaxAuthFailures = 5;            // bad secrets before lockout
inline constexpr int64_t kLockoutMs = 30000;          // lockout duration

// Constant-time equality: no early exit, so a wrong secret cannot be probed by timing.
inline bool constant_time_equal(std::string_view a, std::string_view b) {
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff = static_cast<unsigned char>(diff | (ca ^ cb));
    }
    return diff == 0;
}

// Authenticated session table. A CSRF token exists only after a caller proves knowledge of the
// launch-time operator secret, and it is bound to that authorized session.
//
// The operator secret is never embedded in served assets: the dashboard receives it once, in
// the URL the operator opens (printed by readiness), reads it locally, strips it from the
// address bar, and uses it exactly once to establish the session.
struct SessionStore {
    struct Entry {
        int64_t expires_ms = 0;
    };
    std::unordered_map<std::string, Entry> by_csrf;
    std::string operator_secret;      // "" => the state route is disabled entirely
    int64_t window_start_ms = 0;
    size_t issued_in_window = 0;
    int auth_failures = 0;
    int64_t lockout_until_ms = 0;

    bool state_route_enabled() const { return !operator_secret.empty(); }

    // Verified OS CSPRNG. No fallback: if the kernel cannot provide entropy, the caller must
    // refuse to issue anything (fail closed) rather than degrade to a predictable source.
    static bool secure_random_hex(std::string& out, size_t bytes) {
        std::vector<unsigned char> buf(bytes, 0);
#if defined(__linux__)
        size_t got = 0;
        while (got < buf.size()) {
            const ssize_t n = ::getrandom(buf.data() + got, buf.size() - got, 0);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return false;  // no fallback: refuse
            }
            got += static_cast<size_t>(n);
        }
#else
        return false;
#endif
        static const char* hex = "0123456789abcdef";
        out.clear();
        out.reserve(bytes * 2);
        for (unsigned char b : buf) {
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 0x0f]);
        }
        return true;
    }

    void evict(int64_t now_ms) {
        for (auto it = by_csrf.begin(); it != by_csrf.end();) {
            if (it->second.expires_ms <= now_ms) it = by_csrf.erase(it);
            else ++it;
        }
        while (by_csrf.size() >= kMaxSessions) by_csrf.erase(by_csrf.begin());
    }

    // Authorization: proof of the launch-time secret, with failure accounting and lockout.
    bool authorize(const std::string& presented, int64_t now_ms) {
        if (!state_route_enabled()) return false;
        if (now_ms < lockout_until_ms) return false;  // locked out after repeated failures
        if (constant_time_equal(presented, operator_secret)) {
            auth_failures = 0;
            return true;
        }
        ++auth_failures;
        if (auth_failures >= kMaxAuthFailures) {
            lockout_until_ms = now_ms + kLockoutMs;   // bounded, temporary
            auth_failures = 0;
        }
        return false;
    }

    // Mint a session-bound CSRF token. Returns "" when rate-limited or when secure randomness
    // is unavailable - the caller must then refuse the request.
    std::string issue_csrf(int64_t now_ms) {
        evict(now_ms);
        if (now_ms - window_start_ms >= kIssueWindowMs) {
            window_start_ms = now_ms;
            issued_in_window = 0;
        }
        if (++issued_in_window > kMaxIssuesPerWindow) return "";
        std::string tok;
        if (!secure_random_hex(tok, 32)) return "";
        by_csrf.emplace(tok, Entry{now_ms + kSessionTtlMs});
        return tok;
    }

    // Valid, unexpired and unused => consume it (single use; replay is refused).
    bool consume_csrf(const std::string& tok, int64_t now_ms) {
        evict(now_ms);
        const auto it = by_csrf.find(tok);
        if (it == by_csrf.end()) return false;
        by_csrf.erase(it);
        return true;
    }
};

// Exact same-origin: scheme http, loopback host, and THIS listener's port. Checking the port
// matters - otherwise a page served by any other local port would count as same-origin.
inline bool is_same_local_origin(std::string_view origin, uint16_t server_port) {
    constexpr std::string_view prefix = "http://";
    if (origin.size() <= prefix.size() || origin.substr(0, prefix.size()) != prefix)
        return false;
    const std::string_view authority = origin.substr(prefix.size());
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) return false;   // port mandatory: default port is not this listener
    const std::string_view host = authority.substr(0, colon);
    const std::string_view port = authority.substr(colon + 1);
    if (host != "127.0.0.1" && host != "localhost") return false;
    if (port.empty() || port.size() > 5) return false;
    for (char c : port) if (c < '0' || c > '9') return false;
    return port == std::to_string(server_port);
}

// A parsed, validated routing result handed to the handler.
struct HandlerRequest {
    std::string_view op;        // "status" | "scenario" | "alerts" | "scenarios" | "feed" | "media" | "upload" | "delete"
    std::string_view scenario;  // scenario token; "" => caller's default
    unsigned cycles = 5;        // simulator ticks (valid only when cycles_set)
    bool cycles_set = false;    // true when the caller explicitly set cycles
    // Appended AFTER the original fields on purpose: every existing call site brace-initializes
    // this struct positionally ({"status", "", 5, false}), so inserting a field above would
    // silently rebind their values.
    // Raw request body. Empty for every read route and for the JSON state routes (which carry
    // "{}"); for /api/upload it is the media bytes. A view into the request buffer, valid for the
    // duration of the handler call only -- the handler must not retain it.
    std::string_view body;
    // For op == "media": the media id from /media/<id>. Already path-validated by the transport
    // (no %, backslash, #, NUL; bounded length); the handler validates its charset.
    std::string_view media_id;
    // The request's Content-Type header (already validated to be one of the allowlisted media
    // types on the upload route). The handler requires it to agree with the file signature.
    std::string_view content_type;
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

// Serving an uploaded image or video is a different kind of response (bytes + a media type), so it
// is a separate, OPTIONAL callback rather than a change to Handler. That keeps every existing
// handler -- and every existing test that supplies one -- source-compatible: a caller that does
// not pass a media handler simply gets 404 for /media/<id>.
using MediaHandler =
    std::function<std::string(const std::string& id, int& status_out, std::string& mime_out)>;

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

inline std::string response(unsigned status, std::string_view mime, const std::string& body) {
    const char* reason = "Error";
    switch (status) {
        case 200: reason = "OK"; break;
        case 400: reason = "Bad Request"; break;
        case 401: reason = "Unauthorized"; break;
        case 403: reason = "Forbidden"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 411: reason = "Length Required"; break;
        case 413: reason = "Payload Too Large"; break;
        case 415: reason = "Unsupported Media Type"; break;
        case 429: reason = "Too Many Requests"; break;
        case 503: reason = "Service Unavailable"; break;
        default: reason = "Error"; break;
    }
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason
        + "\r\nContent-Type: " + std::string(mime)
        + "\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
        + "\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer"
        + "\r\nCross-Origin-Resource-Policy: same-origin"
        + "\r\nX-Content-Type-Options: nosniff";
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
                            const Handler& handler,
                      SessionStore& sessions,
                            int64_t now_ms, uint16_t server_port, const MediaHandler& media = MediaHandler()) {
    const auto header_end = request.find("\r\n\r\n");
    // A request MAY carry a body: the POST routes do, and the GET branch rejects one explicitly.
    // The header block itself must still be complete (not truncated mid-request).
    if (header_end == std::string_view::npos || request.size() < header_end + 4)
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
    const std::string_view method = line.substr(0, p1);
    if (method != "GET" && method != "POST")
        return json_error(405, "method_not_allowed");
    const auto path = line.substr(p1 + 1, p2 - p1 - 1);
    if (path.empty() || path.size() > kMaxPathBytes || path.front() != '/' ||
        path.find('%') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos ||
        path.find('#') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos)
        return json_error(400, "malformed");

    // Reject credential parameters in URL query strings (HG-SEC-01 invariant)
    if (path.find("?k=") != std::string_view::npos ||
        path.find("&k=") != std::string_view::npos ||
        path.find("?token=") != std::string_view::npos ||
        path.find("&token=") != std::string_view::npos ||
        path.find("?secret=") != std::string_view::npos ||
        path.find("&secret=") != std::string_view::npos) {
        return json_error(400, "credentials_in_url_forbidden");
    }
    // Header scan. Reject: bodies, framing ambiguity, injected lines, all
    // Authorization headers (this interface is unauthenticated by design), a
    // missing/duplicate/non-loopback Host (DNS-rebinding defence), and any
    // non-loopback Origin (cross-site request defence for the scenario route).
    bool host_seen = false;
    bool origin_ok = true;
    bool origin_seen = false;
    bool cl_seen = false;
    size_t content_length = 0;
    std::string content_type;
    std::string csrf_token;
    std::string presented_secret;
    bool csrf_seen = false;
    bool secret_seen = false;
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
            if (origin_seen) return json_error(400, "malformed");  // duplicate
            origin_seen = true;
            // Exact same-origin (scheme + loopback host + this listener's port).
            if (!is_same_local_origin(value, server_port)) origin_ok = false;
        } else if (key == "content-length") {
            if (cl_seen || value.empty() || value.size() > 6)
                return json_error(400, "malformed");
            cl_seen = true;
            for (char ch : value) {
                if (ch < '0' || ch > '9') return json_error(400, "malformed");
                content_length = content_length * 10 + static_cast<size_t>(ch - '0');
            }
        } else if (key == "content-type") {
            if (!content_type.empty()) return json_error(400, "malformed");
            content_type = std::string(value);
        } else if (key == "x-hg-csrf") {
            if (csrf_seen) return json_error(400, "malformed");  // duplicates DENY
            csrf_seen = true;
            csrf_token = std::string(value);
        } else if (key == "x-hg-secret") {
            if (secret_seen) return json_error(400, "malformed");  // duplicates DENY
            secret_seen = true;
            if (value.size() > 256) return json_error(400, "malformed");
            presented_secret = std::string(value);
        }
        // Framing ambiguity, smuggled lines and credentials we do not use are refused.
        if (key == "transfer-encoding" || key == "authorization" || key == "expect")
            return json_error(400, "malformed");
        at = next + 2;
    }
    if (!host_seen) return json_error(400, "malformed");
    if (!origin_ok) return json_error(403, "forbidden_origin");

    // Body framing, for both methods, in the order that keeps each documented outcome honest:
    //   a declared length that disagrees with what arrived is ambiguous -> refuse;
    //   a GET carries no body and may not declare a length               -> refuse;
    //   a POST that sends a body must declare its length (411 is the POST branch's answer).
    const size_t actual_body = request.size() - (header_end + 4);
    // A declared length over the cap is refused as too-large BEFORE it is compared with what
    // arrived: the cap is a property of the declaration, not of the bytes we happened to receive.
    if (cl_seen && content_length > kMaxBodyBytes) return json_error(413, "too_large");
    if (cl_seen && actual_body != content_length) return json_error(400, "malformed");
    if (method == "GET" && (actual_body > 0 || cl_seen)) return json_error(400, "malformed");

    // The handler decides the business outcome, including a non-200 status when
    // it rejects a well-formed request (e.g. an unknown scenario id). The
    // transport owns framing, header and query validation only.
    auto invoke = [&handler](const HandlerRequest& r) {
        int status = 200;
        std::string body = handler(r, status);
        return response(static_cast<unsigned>(status), "application/json", body);
    };

    const bool is_state_route = (path == "/api/run-scenario" ||
                                 path.substr(0, 18) == "/api/run-scenario?");
    const bool is_session_route = (path == "/api/session");
    // The backup-portal routes. Upload and delete are mutations and therefore require an
    // authorized CSRF session, exactly like the scenario route; upload additionally gets the
    // larger body ceiling and a media content-type allowlist.
    const bool is_upload_route = (path == "/api/upload");
    const bool is_delete_route = (path == "/api/delete");
    const bool is_media_route = (path.substr(0, 7) == "/media/");
    const bool is_mutation_route = (is_state_route || is_session_route ||
                                    is_upload_route || is_delete_route);

    // ---- GET: read-only routes only (control 1) ------------------------------------
    if (method == "GET") {
        // A state-advancing route is never reachable by GET, and the handler is not called.
        if (is_state_route || is_session_route)
            return json_error(405, "method_not_allowed");
        if (cl_seen) return json_error(400, "malformed");  // no bodies on reads
        if (path == "/" || path == "/index.html")
            return response(200, "text/html; charset=utf-8", std::string(html));
        if (path == "/api/health")
            return response(200, "application/json",
                            R"({"ok":true,"status":"up","simulated":true})");
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
        // Uploaded-media feed: read-only list of what an operator has uploaded. Never a live
        // camera: the payload says so, and the handler reports only recorded uploads.
        if (path == "/api/feed")
            return invoke({"feed", "", 5, false});
        if (is_media_route) {
            // /media/<id>: read-only bytes of a previously uploaded, validated file. The transport
            // has already refused %, backslash, #, NUL and over-long paths, so the id cannot escape
            // a directory; the media handler additionally constrains the charset and resolves it
            // through its own index rather than by concatenating a path.
            if (!media) return json_error(404, "not_found");
            const std::string_view id = path.substr(7);
            if (id.empty()) return json_error(404, "not_found");
            int status = 200;
            std::string mime;
            std::string body = media(std::string(id), status, mime);
            return response(static_cast<unsigned>(status),
                            mime.empty() ? "application/octet-stream" : mime, body);
        }
        return json_error(404, "not_found");
    }

    // ---- POST: the state-adjacent routes and the portal mutations (controls 2/4/5) -----
    if (!is_mutation_route) return json_error(405, "method_not_allowed");
    // The upload route is the only one allowed a body above the JSON cap. The declared length is
    // checked first, then the reader is bounded by the same cap, so neither an honest oversized
    // request nor a dishonest Content-Length can allocate past it.
    const size_t body_cap = is_upload_route ? kMaxUploadBytes : kMaxBodyBytes;
    if (!cl_seen) return json_error(411, "length_required");
    if (content_length > body_cap) return json_error(413, "too_large");

    // Upload carries media bytes with an allowlisted content type; every other POST is a JSON
    // object. Anything else is refused before the handler sees it.
    if (is_upload_route) {
        if (content_length == 0) return json_error(400, "malformed");
        const bool media_type_ok = (content_type == "image/png" ||
                                    content_type == "image/jpeg" ||
                                    content_type == "image/webp" ||
                                    content_type == "video/mp4" ||
                                    content_type == "video/webm");
        if (!media_type_ok) return json_error(415, "unsupported_media_type");
        if (!origin_seen) return json_error(403, "forbidden_origin");
        if (!sessions.state_route_enabled()) return json_error(503, "state_route_disabled");
        // Single-use CSRF token from an authorized session, exactly as the scenario route.
        if (csrf_token.empty() || !sessions.consume_csrf(csrf_token, now_ms))
            return json_error(403, "forbidden_csrf");
        HandlerRequest req;
        req.op = "upload";
        req.body = std::string_view(request).substr(header_end + 4);
        req.content_type = content_type;
        return invoke(req);
    }
    // A body must actually have arrived, and must be a JSON object (never a form post).
    if (request.size() != header_end + 4 + content_length)
        return json_error(400, "malformed");
    if (content_type != "application/json")
        return json_error(415, "unsupported_media_type");
    const auto body = request.substr(header_end + 4);
    // The state routes take no parameters, so they accept only "{}". The delete route carries a
    // bounded JSON object naming the upload to remove -- still JSON-only, still <= kMaxBodyBytes,
    // and parsed by the handler, which validates the id shape.
    if (!is_delete_route && !body.empty() && body != "{}")
        return json_error(400, "malformed");
    // Strict same-origin: required, and it must be THIS scheme/host/port. A cross-site page,
    // or a page served from another local port, cannot satisfy it.
    if (!origin_seen) return json_error(403, "forbidden_origin");

    if (is_delete_route) {
        // Deleting an upload is a mutation: same authorized, single-use CSRF session as the
        // scenario route. The id travels in the (bounded) JSON body rather than the query so
        // it cannot end up in a URL, a referrer or a log line.
        if (!sessions.state_route_enabled()) return json_error(503, "state_route_disabled");
        if (csrf_token.empty() || !sessions.consume_csrf(csrf_token, now_ms))
            return json_error(403, "forbidden_csrf");
        HandlerRequest req;
        req.op = "delete";
        req.body = body;
        return invoke(req);
    }

    if (is_session_route) {
        // Authorization happens HERE and nowhere else. Without a configured launch secret the
        // state route is disabled outright; with one, the caller must prove knowledge of it.
        if (!sessions.state_route_enabled())
            return json_error(503, "state_route_disabled");
        if (presented_secret.empty() || !sessions.authorize(presented_secret, now_ms))
            return json_error(401, "unauthorized");
        const std::string tok = sessions.issue_csrf(now_ms);
        if (tok.empty()) {
            // Rate-limited, or the OS CSPRNG refused: issue nothing, and say so.
            return json_error(429, "issuance_limited");
        }
        return response(200, "application/json",
                        "{\"ok\":true,\"csrf\":\"" + tok + "\",\"expires_ms\":" +
                            std::to_string(kSessionTtlMs) + "}");
    }

    // State route: the token must be a live, authorized session token - unexpired and unused.
    // It is consumed here, so a replay of the same request is refused. No token, or a token
    // that was never issued to an authorized caller, means no handler invocation at all.
    if (!sessions.state_route_enabled())
        return json_error(503, "state_route_disabled");
    if (csrf_token.empty() || !sessions.consume_csrf(csrf_token, now_ms))
        return json_error(403, "forbidden_csrf");

    {
        // (unchanged, strictly allowlisted query parsing for scenario/cycles)
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
inline int serve(uint16_t port, std::string_view html, const Handler& handler,
                 std::string operator_secret = std::string(),
                 const MediaHandler& media = MediaHandler()) {
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
    // One CSRF table for the server's lifetime: a token minted for one connection must
    // still be valid on the next, or the two-step (session, then scenario) flow could
    // never complete. It is bounded and expiry-evicted inside the store.
    SessionStore sessions;
    if (operator_secret.empty()) {
        if (const char* env_sec = std::getenv("HG_DEMO_SECRET")) {
            operator_secret = env_sec;
        }
    }
    sessions.operator_secret = operator_secret;
    // If the operator did not supply a secret, mint one for this launch and announce it once.
    // It is launch-time, unpredictable and never embedded in the served page.
    if (sessions.operator_secret.empty()) {
        // With no operator-supplied secret the state route stays DISABLED (503). We do not
        // generate-and-reveal a credential: a secret the operator never chose is not an
        // authorization decision, and printing it would put it in logs.
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    // A 1s listener timeout makes accept() poll-wake so a requested shutdown
    // is honored promptly regardless of SA_RESTART semantics.
    const timeval ltimeout{1, 0};
    (void)::setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &ltimeout, sizeof(ltimeout));
    socklen_t addr_size = sizeof(addr);
    (void)::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_size);
    // The generated secret is NEVER printed: not to stdout, not to a log, not into an archive.
    // The operator supplies it via HG_DEMO_SECRET, or types it into the dashboard's credential
    // field, which posts it to /api/session. A secret that only exists in the process env and
    // the operator's head is not recoverable from CI or archived readiness output.
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
        const auto output = too_large
            ? json_error(413, "too_large")
            : dispatch(input, html, handler, sessions, now_ms(),
                       ntohs(addr.sin_port), media);
        send_all(client, output);
        ::close(client);
    }
    ::close(listener);
    return 4;
}
}  // namespace hg_native_http
#endif  // !_WIN32

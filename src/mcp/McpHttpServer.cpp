#include "mcp/McpHttpServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace homeguardian::mcp {
namespace {

using Clock = std::chrono::steady_clock;

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

std::string reason_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

struct Parsed {
    std::string method;
    std::string path;
    std::string http_version;
    std::unordered_map<std::string, std::string> headers;  // lowercase names
    std::string body;
    bool ok = false;
    bool header_overflow = false;
    bool body_overflow = false;
    bool timed_out = false;
    bool chunked = false;
    bool duplicate_sensitive_header = false;
};

// Fd RAII so no path leaks a descriptor.
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f = -1) : fd(f) {}
    ~ScopedFd() {
        if (fd >= 0) {
            ::shutdown(fd, SHUT_WR);
            char drain[512];
            while (::recv(fd, drain, sizeof(drain), MSG_DONTWAIT) > 0) {}
            ::close(fd);
        }
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    int release() { int f = fd; fd = -1; return f; }
};

bool write_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        // MSG_NOSIGNAL: a client that vanished must not raise SIGPIPE and kill the server.
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EINTR)) continue;
        return false;
    }
    return true;
}

struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    void add(const std::string& k, const std::string& v) { headers.emplace_back(k, v); }
};

void send_response(int fd, const Response& r) {
    std::ostringstream out;
    out << "HTTP/1.1 " << r.status << " " << reason_phrase(r.status) << "\r\n";
    for (const auto& h : r.headers) out << h.first << ": " << h.second << "\r\n";
    out << "Content-Length: " << r.body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << r.body;
    const std::string text = out.str();
    (void)write_all(fd, text.data(), text.size());
}

// Parse one request from an already-connected socket, within strict bounds and deadline.
Parsed read_request(int fd, size_t max_header_bytes, size_t max_body_bytes, int timeout_ms = 3000) {
    Parsed p;
    std::string buf;
    buf.reserve(4096);
    std::array<char, 4096> chunk{};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 3000);
    size_t header_end = std::string::npos;
    while (true) {
        header_end = buf.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (buf.size() > max_header_bytes) {
            p.header_overflow = true;
            return p;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            p.timed_out = true;
            return p;
        }
        int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remaining_ms <= 0) {
            p.timed_out = true;
            return p;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, remaining_ms);
        if (pr == 0) {
            p.timed_out = true;
            return p;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            return p;
        }
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            return p;
        }
        ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) return p;  // client closed
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) p.timed_out = true;
            return p;
        }
        buf.append(chunk.data(), static_cast<size_t>(n));
    }

    const std::string head = buf.substr(0, header_end);
    size_t line_end = head.find("\r\n");
    const std::string request_line = head.substr(0, line_end == std::string::npos ? head.size() : line_end);
    {
        std::istringstream rl(request_line);
        if (!(rl >> p.method >> p.path >> p.http_version)) return p;
        if (p.http_version.rfind("HTTP/1.", 0) != 0) return p;
    }
    static const std::unordered_set<std::string> sensitive_headers = {
        "authorization", "content-length", "transfer-encoding", "origin", "host", "mcp-session-id"
    };

    size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
    while (pos < head.size()) {
        size_t eol = head.find("\r\n", pos);
        const std::string line = head.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? head.size() : eol + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;  // tolerate junk lines only after validation below
        const std::string name = to_lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name.empty()) continue;
        if (sensitive_headers.find(name) != sensitive_headers.end()) {
            if (p.headers.find(name) != p.headers.end()) {
                p.duplicate_sensitive_header = true;
                return p;
            }
        }
        p.headers[name] = value;
    }
    if (p.method.empty() || p.path.empty()) return p;

    if (auto it = p.headers.find("transfer-encoding"); it != p.headers.end()) {
        if (to_lower(it->second).find("chunked") != std::string::npos) {
            p.chunked = true;
            return p;
        }
    }

    size_t content_length = 0;
    if (auto it = p.headers.find("content-length"); it != p.headers.end()) {
        const std::string v = trim(it->second);
        if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) return p;
        try {
            unsigned long long parsed = std::stoull(v);
            if (parsed > max_body_bytes) {
                p.body_overflow = true;
                return p;
            }
            content_length = static_cast<size_t>(parsed);
        } catch (const std::exception&) {
            return p;
        }
    }

    std::string body = buf.substr(header_end + 4);
    while (body.size() < content_length) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            p.timed_out = true;
            return p;
        }
        int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remaining_ms <= 0) {
            p.timed_out = true;
            return p;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, remaining_ms);
        if (pr == 0) {
            p.timed_out = true;
            return p;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            return p;
        }
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            return p;
        }
        ssize_t n = ::recv(fd, chunk.data(), std::min(chunk.size(), content_length - body.size()), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) p.timed_out = true;
            return p;
        }
        body.append(chunk.data(), static_cast<size_t>(n));
    }
    if (body.size() > content_length) body.resize(content_length);
    if (content_length > 0 && body.size() != content_length) return p;

    p.body = std::move(body);
    p.ok = true;
    return p;
}

bool origin_allowed(const std::string& origin, const std::vector<std::string>& allow) {
    for (const auto& a : allow) {
        if (a == origin) return true;
    }
    return false;
}

// Accept header check: the client must be able to take JSON or an SSE stream.
bool accepts_any(const std::string& accept, const std::string& needle) {
    return to_lower(accept).find(needle) != std::string::npos;
}

}  // namespace

bool is_loopback_literal(const std::string& addr) {
    if (addr == "localhost" || addr == "::1") return true;
    if (addr.rfind("127.", 0) == 0) return true;
    return false;
}

bool read_os_entropy(void* buf, size_t len) {
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    size_t total = 0;
    auto* p = static_cast<uint8_t*>(buf);
    while (total < len) {
        ssize_t n = ::read(fd, p + total, len - total);
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

std::string mint_csprng_session_id() {
    std::array<uint8_t, 32> bytes{};
    if (!read_os_entropy(bytes.data(), bytes.size())) {
        spdlog::critical("mcp: OS-backed CSPRNG entropy source failed or unavailable");
        throw std::runtime_error("csprng_entropy_unavailable");
    }
    static const char hex_digits[] = "0123456789abcdef";
    std::string id;
    id.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        id.push_back(hex_digits[(b >> 4) & 0x0f]);
        id.push_back(hex_digits[b & 0x0f]);
    }
    return id;
}

struct McpHttpServer::Impl {
    McpHttpConfig cfg;
    const ToolRegistry& tools;
    const IAlexaCredentialVerifier& verifier;

    Dispatcher dispatcher;
    int listen_fd = -1;

    struct SessionEntry {
        SessionState state;
        Clock::time_point last_seen;
        std::string principal;
        std::string credential_id;
        AlexaAuthContext auth;
    };
    std::mutex mu;
    std::unordered_map<std::string, SessionEntry> sessions;

    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<int> queue;
    bool shutting_down = false;

    mutable std::mutex stats_mu;
    Stats stats_;

    Impl(McpHttpConfig c, const ToolRegistry& t, const IAlexaCredentialVerifier& v)
        : cfg(std::move(c)), tools(t), verifier(v), dispatcher(t) {}

    std::string mint_session_id() {
        return mint_csprng_session_id();
    }

    // Caller must hold mu.
    void evict_locked(Clock::time_point now) {
        const auto idle = std::chrono::seconds(cfg.session_idle_seconds);
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (now - it->second.last_seen > idle) {
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
        while (sessions.size() >= cfg.max_sessions) {
            auto oldest = sessions.begin();
            for (auto it = sessions.begin(); it != sessions.end(); ++it) {
                if (it->second.last_seen < oldest->second.last_seen) oldest = it;
            }
            sessions.erase(oldest);
        }
    }

    void bump(uint64_t Stats::*field) {
        std::lock_guard<std::mutex> lk(stats_mu);
        stats_.*field += 1;
    }
    void bump_requests() {
        std::lock_guard<std::mutex> lk(stats_mu);
        stats_.requests += 1;
    }

    void respond_simple(int fd, int status, const std::string& body_text = std::string(),
                        const std::string& extra_header = std::string(),
                        const std::string& extra_value = std::string()) {
        Response r;
        r.status = status;
        r.add("Content-Type", "application/json");
        if (!extra_header.empty()) r.add(extra_header, extra_value);
        r.body = body_text.empty() ? std::string("{}") : body_text;
        send_response(fd, r);
    }

    void handle_connection(int fd);
    void worker_loop();
};

void McpHttpServer::Impl::handle_connection(int fd) {
    ScopedFd sock(fd);
    struct timeval tv {};
    tv.tv_sec = cfg.read_timeout_ms / 1000;
    tv.tv_usec = (cfg.read_timeout_ms % 1000) * 1000;
    ::setsockopt(sock.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct timeval wv {};
    wv.tv_sec = cfg.write_timeout_ms / 1000;
    wv.tv_usec = (cfg.write_timeout_ms % 1000) * 1000;
    ::setsockopt(sock.fd, SOL_SOCKET, SO_SNDTIMEO, &wv, sizeof(wv));

    Parsed req = read_request(sock.fd, cfg.max_header_bytes, cfg.max_body_bytes, cfg.read_timeout_ms);
    if (req.header_overflow) {
        bump(&Stats::rejected_oversize);
        respond_simple(sock.fd, 431);
        return;
    }
    if (req.body_overflow) {
        bump(&Stats::rejected_oversize);
        respond_simple(sock.fd, 413);
        return;
    }
    if (req.timed_out) {
        bump(&Stats::errors);
        respond_simple(sock.fd, 408);
        return;
    }
    if (req.chunked) {
        bump(&Stats::errors);
        respond_simple(sock.fd, 501,
                       make_error_json("null", RpcCode::invalid_request,
                                       "Invalid Request: chunked bodies not supported"));
        return;
    }
    if (req.duplicate_sensitive_header) {
        bump(&Stats::errors);
        respond_simple(sock.fd, 400);
        return;
    }
    if (!req.ok) {
        bump(&Stats::errors);
        respond_simple(sock.fd, 400);
        return;
    }

    bump_requests();

    // ---- path routing -------------------------------------------------------------
    const bool is_mcp_path = (req.path == "/mcp" || req.path == "/mcp/");
    if (!is_mcp_path) {
        respond_simple(sock.fd, 404);
        return;
    }

    // ---- Origin validation (DNS-rebinding defence) --------------------------------
    if (auto it = req.headers.find("origin"); it != req.headers.end()) {
        if (!origin_allowed(trim(it->second), cfg.allowed_origins)) {
            bump(&Stats::rejected_origin);
            spdlog::warn("mcp: rejected request with a disallowed Origin");
            respond_simple(sock.fd, 403);
            return;
        }
    }

    // ---- authentication -----------------------------------------------------------
    std::string credential;
    AlexaAuthContext auth;
    if (cfg.require_auth) {
        auto it = req.headers.find("authorization");
        if (it == req.headers.end()) {
            bump(&Stats::rejected_auth);
            respond_simple(sock.fd, 401, "{}", "WWW-Authenticate", "Bearer");
            return;
        }
        std::string value = trim(it->second);
        const std::string lower_value = to_lower(value);
        if (lower_value.rfind("bearer ", 0) != 0) {
            bump(&Stats::rejected_auth);
            respond_simple(sock.fd, 401, "{}", "WWW-Authenticate", "Bearer");
            return;
        }
        credential = trim(value.substr(7));
        if (credential.empty() || credential.size() > ALEXA_MAX_TOKEN_CHARS) {
            bump(&Stats::rejected_auth);
            respond_simple(sock.fd, 401, "{}", "WWW-Authenticate", "Bearer");
            return;
        }
        auth = verifier.verify(credential, std::chrono::system_clock::now());
        if (!auth.authenticated) {
            bump(&Stats::rejected_auth);
            // No detail: the client learns only that the credential did not authenticate.
            spdlog::warn("mcp: rejected an unauthenticated request");
            respond_simple(sock.fd, 401, "{}", "WWW-Authenticate", "Bearer");
            return;
        }
    }

    // ---- method dispatch ----------------------------------------------------------
    if (req.method == "GET") {
        Response r;
        r.status = 405;
        r.add("Allow", "POST, DELETE");
        r.add("Content-Type", "application/json");
        r.body = make_error_json("null", RpcCode::invalid_request,
                                 "Invalid Request: this endpoint offers no server-initiated stream");
        send_response(sock.fd, r);
        return;
    }

    const std::string sid = [&] {
        auto it = req.headers.find("mcp-session-id");
        return it == req.headers.end() ? std::string() : trim(it->second);
    }();

    if (req.method == "DELETE") {
        if (sid.empty()) {
            respond_simple(sock.fd, 400);
            return;
        }
        std::lock_guard<std::mutex> lk(mu);
        const Clock::time_point now = Clock::now();
        evict_locked(now);
        auto it = sessions.find(sid);
        if (it == sessions.end()) {
            respond_simple(sock.fd, 404);
            return;
        }
        if (cfg.require_auth) {
            if (it->second.credential_id.empty() || auth.credential_id.empty() ||
                it->second.credential_id != auth.credential_id ||
                it->second.principal.empty() || auth.principal.empty() ||
                it->second.principal != auth.principal) {
                spdlog::warn("mcp: principal '{}' attempted to delete session '{}' owned by '{}'",
                             auth.principal, sid, it->second.principal);
                bump(&Stats::rejected_auth);
                respond_simple(sock.fd, 403);
                return;
            }
        }
        sessions.erase(it);
        respond_simple(sock.fd, 200);
        return;
    }

    if (req.method != "POST") {
        Response r;
        r.status = 405;
        r.add("Allow", "POST, DELETE");
        r.add("Content-Type", "application/json");
        r.body = "{}";
        send_response(sock.fd, r);
        return;
    }

    // POST-only content rules.
    const std::string content_type = [&] {
        auto it = req.headers.find("content-type");
        return it == req.headers.end() ? std::string() : to_lower(it->second);
    }();
    if (content_type.rfind("application/json", 0) != 0) {
        respond_simple(sock.fd, 415);
        return;
    }
    if (req.headers.find("content-length") == req.headers.end()) {
        respond_simple(sock.fd, 411);
        return;
    }
    const std::string accept = [&] {
        auto it = req.headers.find("accept");
        return it == req.headers.end() ? std::string() : it->second;
    }();
    const bool wants_json = accepts_any(accept, "application/json");
    const bool wants_sse = accepts_any(accept, "text/event-stream");
    if (!accept.empty() && !wants_json && !wants_sse) {
        respond_simple(sock.fd, 406);
        return;
    }

    // MCP-Protocol-Version is required after initialization; if it is present it must be
    // one we speak, otherwise the client is told rather than being silently downgraded.
    if (auto it = req.headers.find("mcp-protocol-version"); it != req.headers.end()) {
        const std::string v = trim(it->second);
        if (!v.empty() && !Dispatcher::version_supported(v)) {
            bump(&Stats::errors);
            respond_simple(sock.fd, 400,
                           make_error_json("null", RpcCode::invalid_request,
                                           "Invalid Request: unsupported protocol version"));
            return;
        }
    }

    // ---- session resolution -------------------------------------------------------
    SessionState state;
    bool session_present = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        const Clock::time_point now = Clock::now();
        evict_locked(now);
        if (!sid.empty()) {
            auto it = sessions.find(sid);
            if (it == sessions.end()) {
                // An expired or unknown session is a 404 so the client re-initializes.
                respond_simple(sock.fd, 404);
                return;
            }
            if (cfg.require_auth) {
                if (it->second.credential_id.empty() || auth.credential_id.empty() ||
                    it->second.credential_id != auth.credential_id ||
                    it->second.principal.empty() || auth.principal.empty() ||
                    it->second.principal != auth.principal) {
                    spdlog::warn("mcp: principal '{}' attempted to access session '{}' owned by '{}'",
                                 auth.principal, sid, it->second.principal);
                    bump(&Stats::rejected_auth);
                    respond_simple(sock.fd, 403);
                    return;
                }
            }
            it->second.last_seen = now;
            state = it->second.state;
            session_present = true;
        }
    }

    // ---- dispatch -----------------------------------------------------------------
    DispatchOutcome outcome = dispatcher.handle(req.body, state, credential);

    std::string new_session_id;
    if (outcome.started_session) {
        std::lock_guard<std::mutex> lk(mu);
        const Clock::time_point now = Clock::now();
        evict_locked(now);
        if (session_present) {
            new_session_id = sid;
            sessions[sid] = SessionEntry{state, now, auth.principal, auth.credential_id, auth};
        } else {
            try {
                for (int attempt = 0; attempt < 5; ++attempt) {
                    std::string cand = mint_session_id();
                    if (sessions.find(cand) == sessions.end()) {
                        new_session_id = std::move(cand);
                        break;
                    }
                }
            } catch (const std::exception& e) {
                spdlog::critical("mcp: failed to mint session ID: {}", e.what());
                respond_simple(sock.fd, 500);
                return;
            }
            if (new_session_id.empty()) {
                spdlog::critical("mcp: session ID collision loop exhausted");
                respond_simple(sock.fd, 500);
                return;
            }
            sessions[new_session_id] = SessionEntry{state, now, auth.principal, auth.credential_id, auth};
        }
    } else if (session_present) {
        std::lock_guard<std::mutex> lk(mu);
        sessions[sid] = SessionEntry{state, Clock::now(), auth.principal, auth.credential_id, auth};
    }

    // A stateful server is required to reject requests that arrive without a session once
    // one has been issued. The dispatcher already produced exactly that error, so it is
    // translated to the transport status the specification asks for.
    if (!session_present && outcome.has_body &&
        outcome.body.find("initialize must be called first") != std::string::npos) {
        Response r;
        r.status = 400;
        r.add("Content-Type", "application/json");
        r.body = outcome.body;
        send_response(sock.fd, r);
        return;
    }

    if (!outcome.has_body) {
        Response r;
        r.status = 202;
        if (!new_session_id.empty()) r.add("Mcp-Session-Id", new_session_id);
        send_response(sock.fd, r);
        return;
    }

    Response r;
    r.status = 200;
    if (!new_session_id.empty()) r.add("Mcp-Session-Id", new_session_id);
    if (wants_json || accept.empty()) {
        r.add("Content-Type", "application/json");
        r.body = outcome.body;
    } else {
        // The client accepts only text/event-stream: frame the single reply as one SSE
        // message and close, which is valid for a request that needs no streaming.
        r.add("Content-Type", "text/event-stream");
        r.add("Cache-Control", "no-cache");
        r.body = "event: message\ndata: " + outcome.body + "\n\n";
    }
    send_response(sock.fd, r);
}

void McpHttpServer::Impl::worker_loop() {
    while (true) {
        int fd = -1;
        {
            std::unique_lock<std::mutex> lk(q_mu);
            q_cv.wait(lk, [this] { return shutting_down || !queue.empty(); });
            if (shutting_down && queue.empty()) return;
            fd = queue.front();
            queue.pop_front();
        }
        try {
            handle_connection(fd);
        } catch (const std::exception& e) {
            bump(&Stats::errors);
            spdlog::error("mcp: connection handler error: {}", e.what());
        } catch (...) {
            bump(&Stats::errors);
            spdlog::error("mcp: connection handler error");
        }
    }
}

McpHttpServer::McpHttpServer(McpHttpConfig cfg,
                             const ToolRegistry& tools,
                             const IAlexaCredentialVerifier& verifier)
    : impl_(std::make_unique<Impl>(std::move(cfg), tools, verifier)) {}

McpHttpServer::~McpHttpServer() {
    if (impl_ && impl_->listen_fd >= 0) ::close(impl_->listen_fd);
}

bool McpHttpServer::start(std::string& error) {
    if (impl_->cfg.bind_address == "0.0.0.0" || impl_->cfg.bind_address == "::" || impl_->cfg.bind_address == "0") {
        error = "refusing to bind wildcard address (0.0.0.0); explicit interface or loopback required";
        return false;
    }
    if (!impl_->cfg.allow_remote && !is_loopback_literal(impl_->cfg.bind_address)) {
        error = "refusing to bind a non-loopback address without allow_remote";
        return false;
    }
    if (impl_->cfg.require_auth == false) {
        error = "refusing to start without authentication";
        return false;
    }
    ScopedFd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd.fd < 0) {
        error = "socket() failed";
        return false;
    }
    int one = 1;
    ::setsockopt(fd.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(impl_->cfg.port);
    if (::inet_pton(AF_INET, impl_->cfg.bind_address.c_str(), &addr.sin_addr) != 1) {
        error = "invalid bind address";
        return false;
    }
    if (::bind(fd.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "bind failed";
        return false;
    }
    if (::listen(fd.fd, 64) != 0) {
        error = "listen failed";
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd.fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    }
    impl_->listen_fd = fd.release();
    return true;
}

void McpHttpServer::serve(std::atomic<bool>& stop) {
    std::vector<std::thread> workers;
    workers.reserve(impl_->cfg.workers);
    for (size_t i = 0; i < impl_->cfg.workers; ++i) {
        workers.emplace_back([this] { impl_->worker_loop(); });
    }

    while (!stop.load()) {
        pollfd pfd{};
        pfd.fd = impl_->listen_fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 200);  // 200 ms so the stop flag is noticed promptly
        if (pr <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int client = ::accept(impl_->listen_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (client < 0) continue;

        bool accepted = false;
        {
            std::lock_guard<std::mutex> lk(impl_->q_mu);
            if (!impl_->shutting_down && impl_->queue.size() < impl_->cfg.max_inflight) {
                impl_->queue.push_back(client);
                accepted = true;
            }
        }
        if (accepted) {
            impl_->q_cv.notify_one();
        } else {
            // Bounded: past the in-flight cap the server sheds load instead of queueing
            // without limit. A tiny best-effort 503 is sent; if that fails, just close.
            impl_->bump(&Stats::rejected_too_many);
            Response r;
            r.status = 503;
            r.add("Content-Type", "application/json");
            r.add("Retry-After", "1");
            r.body = "{}";
            send_response(client, r);
            ::shutdown(client, SHUT_WR);
            char drain_buf[512];
            while (::recv(client, drain_buf, sizeof(drain_buf), MSG_DONTWAIT) > 0) {}
            ::close(client);
        }
    }

    {
        std::lock_guard<std::mutex> lk(impl_->q_mu);
        impl_->shutting_down = true;
    }
    impl_->q_cv.notify_all();
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
    spdlog::info("mcp: server stopped");
}

void McpHttpServer::set_allowed_origins(const std::vector<std::string>& origins) {
    impl_->cfg.allowed_origins = origins;
}

McpHttpServer::Stats McpHttpServer::stats() const {
    std::lock_guard<std::mutex> lk(impl_->stats_mu);
    return impl_->stats_;
}

}  // namespace homeguardian::mcp

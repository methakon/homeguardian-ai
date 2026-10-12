#pragma once
// HomeGuardian native demo HTTP transport (Linux/POSIX).
// Separate, opt-in component: it does not alter the existing C++ bridge,
// consent gate, dashboard, or currently working demo.
// Loopback ONLY; GET-only; one request at a time; no dependencies beyond libc++.
// This limited HTTP/1.1 subset is NOT a general-purpose/public HTTP server.
#ifndef _WIN32
#include <arpa/inet.h>
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
using Handler = std::function<std::string(std::string_view, unsigned)>;
// Handler receives "status" or "scenario". It must return JSON. The native
// app integration should call the existing C++ backend, never synthesize data.
inline std::string response(unsigned status, std::string_view mime, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request"
        : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed"
        : status == 413 ? "Payload Too Large" : "Service Unavailable";
    return "HTTP/1.1 " + std::to_string(status) + " " + reason
        + "\r\nContent-Type: " + std::string(mime)
        + "\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
          "\r\nConnection: close\r\n\r\n" + body;
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
    if (first_end == std::string_view::npos)
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
    if (path.empty() || path.size() > 128 || path.front() != '/' ||
        path.find('%') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos ||
        path.find('#') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos)
        return json_error(400, "malformed");
    // Reject body framing, ambiguity, injected lines, and all Authorization
    // headers. This is an unauthenticated simulator-only local demo interface.
    bool host_seen = false;
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
        if (key == "host") {
            if (host_seen) return json_error(400, "malformed");
            host_seen = true;
        }
        if (key == "content-length" || key == "transfer-encoding" ||
            key == "authorization" || key == "expect")
            return json_error(400, "malformed");
        at = next + 2;
    }
    if (!host_seen) return json_error(400, "malformed");
    if (path == "/" || path == "/index.html")
        return response(200, "text/html; charset=utf-8", std::string(html));
    if (path == "/api/health")
        return response(200, "application/json",
                        R"({"ok":true,"status":"up","simulated":true})");
    if (path == "/api/status")
        return response(200, "application/json", handler("status", 0));
    if (path == "/api/run-scenario" ||
        path.substr(0, 25) == "/api/run-scenario?cycles=") {
        unsigned cycles = 5;
        if (path != "/api/run-scenario") {
            const auto value = path.substr(25);
            if (value.empty() || value.size() > 3)
                return json_error(400, "invalid_cycles");
            cycles = 0;
            for (char ch : value) {
                if (ch < '0' || ch > '9')
                    return json_error(400, "invalid_cycles");
                cycles = cycles * 10 + static_cast<unsigned>(ch - '0');
            }
            if (cycles > 100) return json_error(400, "invalid_cycles");
        }
        return response(200, "application/json", handler("scenario", cycles));
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
// Synchronous, bounded, loopback-only server. Handler executes on the
// caller thread. Intended solely for a controlled local simulator demo.
// Port=0 chooses an ephemeral port and announces "DEMO_READY <port>".
// Returns only if bind/listen/accept fails; process owner handles shutdown.
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
    socklen_t addr_size = sizeof(addr);
    (void)::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_size);
    std::cout << "DEMO_READY " << ntohs(addr.sin_port) << std::endl;
    for (;;) {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const timeval timeout{3, 0};
        (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string input;
        char buffer[2048];
        bool too_large = false;
        while (input.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            if (input.size() + static_cast<size_t>(n) > 8192) {
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

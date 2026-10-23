#include "HttpClient.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <string>

namespace homeguardian {

bool HttpPolicy::scheme_supported(const std::string& scheme) { return scheme == "http"; }

HttpClient::Url HttpClient::parse_url(const std::string& url) {
    Url out;
    if (url.empty()) {
        out.error = "empty url";
        return out;
    }
    if (url.size() > HttpPolicy::kMaxUrlChars) {
        out.error = "url too long";
        return out;
    }

    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        out.error = "url has no scheme";
        return out;
    }
    out.scheme = url.substr(0, scheme_end);
    if (out.scheme == "https") {
        // Explicit refusal beats a silent downgrade.
        out.error = "https is not supported: this build links no TLS implementation";
        return out;
    }
    if (!HttpPolicy::scheme_supported(out.scheme)) {
        out.error = "unsupported scheme '" + out.scheme + "'";
        return out;
    }

    std::string rest = url.substr(scheme_end + 3);
    if (rest.empty()) {
        out.error = "url has no host";
        return out;
    }
    const size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (out.path.size() > HttpPolicy::kMaxPathChars) {
        out.error = "path too long";
        return out;
    }
    if (authority.empty()) {
        out.error = "url has no host";
        return out;
    }

    const size_t colon = authority.find(':');
    std::string port_text;
    if (colon == std::string::npos) {
        out.host = authority;
        out.port = 80;
    } else {
        out.host = authority.substr(0, colon);
        port_text = authority.substr(colon + 1);
    }
    if (out.host.empty()) {
        out.error = "url has no host";
        return out;
    }
    if (!port_text.empty()) {
        if (port_text.size() > 5) {
            out.error = "port too long";
            return out;
        }
        for (char c : port_text) {
            if (c < '0' || c > '9') {
                out.error = "port is not numeric";
                return out;
            }
        }
        const long port = std::strtol(port_text.c_str(), nullptr, 10);
        if (port <= 0 || port > 65535) {
            out.error = "port out of range";
            return out;
        }
        out.port = static_cast<uint16_t>(port);
    }

    out.valid = true;
    return out;
}

std::string HttpClient::build_request_head(
    const Url& url, const std::vector<std::pair<std::string, std::string>>& headers,
    std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return std::string();
    };
    if (!url.valid) return fail("invalid url");
    if (url.path.find(' ') != std::string::npos) return fail("path contains a space");

    std::string host_header = url.host;
    if (url.port != 80) host_header += ":" + std::to_string(url.port);

    std::string head = "GET " + url.path + " HTTP/1.1\r\n";
    head += "Host: " + host_header + "\r\n";
    head += "Accept: application/json\r\n";
    head += "Connection: close\r\n";
    head += "User-Agent: homeguardian-hub/1.0\r\n";

    for (const auto& h : headers) {
        // Header injection guard: a CR/LF in a name or value is never legal here.
        if (h.first.empty() || h.second.empty()) return fail("empty header name or value");
        if (h.second.size() > HttpPolicy::kMaxHeaderValueChars) return fail("header value too long");
        const std::string needle = "\r\n";
        if (h.first.find('\r') != std::string::npos || h.first.find('\n') != std::string::npos ||
            h.second.find(needle[0]) != std::string::npos ||
            h.second.find(needle[1]) != std::string::npos) {
            return fail("header contains a line break");
        }
        head += h.first + ": " + h.second + "\r\n";
    }
    head += "\r\n";
    if (head.size() > HttpPolicy::kMaxRequestBytes) return fail("request too large");
    if (error != nullptr) error->clear();
    return head;
}

bool HttpClient::parse_status_line(const std::string& head, int* status) {
    if (head.size() < 12) return false;
    if (head.compare(0, 5, "HTTP/") != 0) return false;
    const size_t space = head.find(' ');
    if (space == std::string::npos) return false;
    size_t i = space + 1;
    if (i + 3 > head.size()) return false;
    int code = 0;
    size_t digits = 0;
    while (i < head.size() && head[i] >= '0' && head[i] <= '9' && digits < 4) {
        code = code * 10 + (head[i] - '0');
        ++i;
        ++digits;
    }
    if (digits < 3) return false;
    if (status != nullptr) *status = code;
    return true;
}

HttpResponse HttpClient::get(
    const Url& url, const std::vector<std::pair<std::string, std::string>>& headers) const {
    HttpResponse response;
    if (!url.valid) {
        response.error = "invalid url: " + url.error;
        return response;
    }

    std::string build_error;
    const std::string request = build_request_head(url, headers, &build_error);
    if (request.empty()) {
        response.error = build_error.empty() ? "could not build request" : build_error;
        return response;
    }

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    const std::string port_text = std::to_string(url.port);
    const int gai = ::getaddrinfo(url.host.c_str(), port_text.c_str(), &hints, &result);
    if (gai != 0 || result == nullptr) {
        response.error = "host resolution failed";
        return response;
    }

    int fd = -1;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv {};
        tv.tv_sec = cfg_.timeout_ms / 1000;
        tv.tv_usec = (cfg_.timeout_ms % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);

    if (fd < 0) {
        response.error = "connection failed";
        return response;
    }

    if (cfg_.before_send && !cfg_.before_send()) {
        response.error = "request authorization denied";
        ::close(fd);
        return response;
    }
    size_t sent = 0;
    while (sent < request.size()) {
        if (cfg_.before_send && !cfg_.before_send()) {
            response.error = "request authorization denied";
            ::close(fd);
            return response;
        }
        const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            response.error = "send failed";
            ::close(fd);
            return response;
        }
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) break;  // peer closed: normal end for Connection: close
        if (n < 0) {
            if (raw.empty()) {
                response.error = "receive failed";
                ::close(fd);
                return response;
            }
            break;  // partial body is reported with whatever arrived
        }
        raw.append(buffer, static_cast<size_t>(n));
        if (raw.size() > cfg_.max_response_bytes) {
            raw.resize(cfg_.max_response_bytes);
            response.truncated = true;
            break;
        }
    }
    ::close(fd);
    response.bytes_received = raw.size();

    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        response.error = "malformed response (no header terminator)";
        return response;
    }
    const std::string head = raw.substr(0, head_end);
    int status = 0;
    if (!parse_status_line(head, &status)) {
        response.error = "malformed status line";
        return response;
    }
    response.status = status;
    response.body = raw.substr(head_end + 4);
    response.ok = status >= 200 && status < 300;
    // Server header: scanned line by line inside the header block only (never the body, which is
    // caller data). Case-insensitive name, trimmed value, bounded length - a hostile peer cannot
    // make this allocate past kMaxHeaderValueChars.
    {
        size_t at = head.find("\r\n");
        at = (at == std::string::npos) ? head.size() : at + 2;
        while (at < head.size()) {
            const size_t nl = head.find("\r\n", at);
            const std::string line =
                head.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = (nl == std::string::npos) ? head.size() : nl + 2;
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) continue;
            std::string name = line.substr(0, colon);
            for (char& c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name != "server") continue;
            size_t v = colon + 1;
            while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
            response.server = line.substr(v, HttpPolicy::kMaxHeaderValueChars);
            break;
        }
    }
    return response;
}

}  // namespace homeguardian

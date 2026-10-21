#pragma once

// A minimal HTTP/1.1 GET client for the Home Assistant adapter.
//
// WHY HAND-ROLLED: the project deliberately has no third-party networking
// dependency (cpp-httplib was evaluated and rejected), it already hand-writes its
// loopback HTTP server, and the requirement here is one read-only verb against a
// LAN address. This file is that, and nothing more.
//
// SECURITY POSTURE (deliberate, and narrow):
//   * PLAINTEXT ONLY. No TLS implementation is linked into this binary, so an
//     `https://` URL is REFUSED rather than silently downgraded. Callers must
//     opt in explicitly to a plaintext endpoint (`allow_plaintext`), which is why
//     the Home Assistant integration ships disabled by default.
//   * Header values are validated: a value containing CR/LF is rejected, so a
//     hostile or corrupted value cannot inject a header or a second request.
//   * The response is read with a hard byte cap and a socket timeout, so a
//     malicious or broken peer cannot exhaust memory or hang the hub.
//   * NOTHING is logged here. Not the request, not the response, and above all
//     not the Authorization header, which the adapter attaches outside this file.
//   * GET only. There is no POST/PUT/DELETE path, so this client cannot actuate
//     anything on the peer.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace homeguardian {

struct HttpPolicy {
    static constexpr size_t kMaxResponseBytes = 64u * 1024u;  // 64 KiB
    static constexpr size_t kMaxRequestBytes = 4u * 1024u;
    static constexpr size_t kMaxUrlChars = 256;
    static constexpr size_t kMaxPathChars = 512;
    static constexpr size_t kMaxHeaderValueChars = 512;
    static constexpr int kDefaultTimeoutMs = 3000;

    // Only this scheme is transportable by this build.
    static bool scheme_supported(const std::string& scheme);
};

struct HttpResponse {
    bool ok = false;
    int status = 0;              // 0 when the request never completed
    std::string body;
    std::string error;           // bounded, non-sensitive
    size_t bytes_received = 0;
    bool truncated = false;      // peer exceeded the cap; body holds the prefix
};

class HttpClient {
public:
    struct Config {
        int timeout_ms = HttpPolicy::kDefaultTimeoutMs;
        size_t max_response_bytes = HttpPolicy::kMaxResponseBytes;
        Config() = default;
    };

    struct Url {
        bool valid = false;
        std::string scheme;   // "http"
        std::string host;     // hostname or IPv4 literal
        uint16_t port = 0;
        std::string path;     // starts with '/'
        std::string error;    // why it is invalid
    };

    static Config defaults() { return Config{}; }
    explicit HttpClient(Config cfg = defaults()) : cfg_(cfg) {}

    // Parse "http://host[:port][/path]". Rejects: non-http schemes (explicitly,
    // with a TLS message), empty host, oversized input, bad ports.
    static Url parse_url(const std::string& url);

    // Build the request head. Returns empty string when a header value is
    // unusable (CR/LF/oversize) - the caller must not send anything then.
    static std::string build_request_head(
        const Url& url, const std::vector<std::pair<std::string, std::string>>& headers,
        std::string* error);

    // Parse "HTTP/1.1 200 OK" style status lines. Returns false when malformed.
    static bool parse_status_line(const std::string& head, int* status);

    // Perform the GET. Blocking, bounded, no retries (the adapter owns policy).
    HttpResponse get(const Url& url,
                     const std::vector<std::pair<std::string, std::string>>& headers) const;

private:
    Config cfg_;
};

}  // namespace homeguardian

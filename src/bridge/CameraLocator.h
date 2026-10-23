#pragma once

// CameraLocator — MAC-anchored rediscovery of a camera whose DHCP lease moved.
//
// THE PROBLEM
//   The owner's exterior camera is a V380-class device on the home LAN. It holds a DHCP
//   lease, so its address changes without the device changing. A configuration that stores
//   the address therefore rots: the portal keeps fetching a host that is now either empty or
//   - worse - a different device. This module re-attaches the configured CAMERA to a new
//   address, and nothing else.
//
// WHAT IDENTITY IS, AND WHAT IT IS NOT
//   Identity is the camera's explicitly configured MAC address, resolved through
//   the kernel's ARP table. The IPv4 address is an ATTACHMENT attribute: resolved at runtime,
//   never authorization-bearing. An explicit MAC match is required for a discovery candidate.
//   HTTP server strings and JPEG framing are diagnostic only, never authorization.
//
// NO CREDENTIAL SPRAY, EVER
//   The safety rule this module encodes, in code, is:
//     * Camera credentials are NEVER sent by a discovery probe. `LocatorIo` has no credential
//       parameter at all, so no probe can carry one even by mistake.
//     * "Prining" the ARP table is done with a bare TCP connect to the camera's own port. No
//       payload, no HTTP, no auth. A connect that is refused or times out tells us nothing and
//       sends nothing.
//     * Only a host already identified as the camera may later be handed credentials, and that
//       is the caller's (LiveCameraFeed's) job, AFTER this module says `found`.
//   A subnet sweep of authenticated requests - which is what a naive "find the camera" loop
//   does - is not implemented and cannot be reached from this API.
//
// BOUNDED BY CONSTRUCTION
//   * The address range is derived from the CONFIGURED host's own address, never from an
//     argument: exactly that /24, network and broadcast excluded => 254 hosts, hard cap.
//   * <= 16 concurrent connects, 150-300 ms per-host connect timeout, ~5 s overall deadline.
//     The deadline is checked between waves and inside each wave, so a wedged host cannot
//     extend the scan.
//   * /proc/net/arp is READ (bounded bytes, bounded entries), never shelled out to. No
//     subprocess, no `arp -a`, no parsing of another tool's output format changes.
//   * Rediscovery is rate limited to once per 60 s and the scan time is recorded, so a failed
//     discovery cannot spin: the next attempt is a minute away even if the camera stays gone.
//
// TESTABILITY
//   Every effect is injected: the ARP file path (the file TEXT is supplied), the connect
//   function, the fingerprint fetch and the clock. The unit tests therefore run fully offline
//   against synthetic `/proc/net/arp` fixtures and loopback peers, and never touch a real
//   camera or the owner's LAN.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "camera/DeviceFingerprint.h"

namespace homeguardian {
namespace portal {

// ---------------------------------------------------------------------------------------------
// Bounds. Every one of these is enforced, not advisory.
// ---------------------------------------------------------------------------------------------
struct LocatorPolicy {
    static constexpr int kMaxHosts = 254;             // one /24, minus network and broadcast
    static constexpr int kMaxConcurrentProbes = 16;   // simultaneous TCP connects
    static constexpr int kProbeTimeoutMs = 220;       // inside the 150-300 ms band
    static constexpr int kOverallDeadlineMs = 5000;   // hard scan deadline
    static constexpr int kMinScanIntervalMs = 60000;  // at most one rediscovery per minute
    static constexpr size_t kMaxArpBytes = 256u * 1024u;   // /proc/net/arp is a few KiB
    static constexpr size_t kMaxArpEntries = 1024;
    static constexpr size_t kMaxStateBytes = 512;     // the opaque state file
    static constexpr size_t kMaxHostChars = 64;

    int max_hosts = kMaxHosts;
    int max_concurrent_probes = kMaxConcurrentProbes;
    int probe_timeout_ms = kProbeTimeoutMs;
    int overall_deadline_ms = kOverallDeadlineMs;
    int min_scan_interval_ms = kMinScanIntervalMs;
    size_t max_arp_bytes = kMaxArpBytes;
    size_t max_arp_entries = kMaxArpEntries;
    // How many ARP-present hosts the no-MAC fingerprint fallback may probe. The fallback is
    // unauthenticated, but it is still work: it is capped separately and kept small.
    size_t max_fingerprint_hosts = 16;
};

// ---------------------------------------------------------------------------------------------
// IPv4 helpers. Strict on purpose: an address is parsed, never guessed.
// ---------------------------------------------------------------------------------------------
inline bool parse_ipv4(std::string_view text, uint32_t* out) {
    if (out == nullptr || text.empty() || text.size() > 15) return false;
    uint32_t value = 0;
    int parts = 0;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t start = i;
        unsigned octet = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 3) {
            octet = octet * 10 + static_cast<unsigned>(text[i] - '0');
            ++i;
        }
        if (i == start || octet > 255) return false;                 // empty or out-of-range
        value = (value << 8) | octet;
        ++parts;
        if (i == text.size()) break;
        if (text[i] != '.' || parts == 4) return false;
        ++i;
    }
    if (parts != 4) return false;
    *out = value;
    return true;
}

inline bool is_ipv4_literal(std::string_view text) {
    uint32_t ignored = 0;
    return parse_ipv4(text, &ignored);
}

inline std::string format_ipv4(uint32_t value) {
    return std::to_string((value >> 24) & 0xFF) + "." + std::to_string((value >> 16) & 0xFF) +
           "." + std::to_string((value >> 8) & 0xFF) + "." + std::to_string(value & 0xFF);
}

// The /24 that CONTAINS the given address. 0.0.0.0/24 and 255.x are refused: they are not a
// site network and scanning them would be an arbitrary range, which this module never does.
inline std::optional<uint32_t> subnet24_of(uint32_t address) {
    const uint32_t base = address & 0xFFFFFF00u;
    const uint32_t first = (address >> 24) & 0xFF;
    if (first == 0u || first == 255u) return std::nullopt;
    return base;
}

inline bool same_subnet24(uint32_t a, uint32_t b) {
    return (a & 0xFFFFFF00u) == (b & 0xFFFFFF00u);
}

// ---------------------------------------------------------------------------------------------
// /proc/net/arp parsing.
//
// Format (kernel, net/ipv4/arp.c):
//   IP address       HW type     Flags       HW address            Mask     Device
//   192.168.1.20     0x1         0x2         a4:2b:8c:11:22:33     *        wlan0
//
// An INCOMPLETE entry is real and common - it appears the moment the kernel starts resolving a
// neighbour and has no reply yet. It carries flags 0x0 and the all-zero HW address. Treating
// that as a device would invent an identity, so it is parsed and marked unusable.
// ---------------------------------------------------------------------------------------------
struct ArpEntry {
    std::string ip;
    std::string hw;        // exactly as the table reported it (diagnostics only)
    std::string mac;       // canonical aa:bb:cc:dd:ee:ff, or "" when the value is not a MAC
    std::string device;
    uint32_t address = 0;
    bool usable = false;   // valid unicast IPv4 AND a real (non-placeholder) MAC
};

inline std::vector<std::string> split_ws(std::string_view line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > start) out.emplace_back(line.substr(start, i - start));
    }
    return out;
}

inline std::vector<ArpEntry> parse_arp(std::string_view text,
                                       size_t max_entries = LocatorPolicy::kMaxArpEntries) {
    std::vector<ArpEntry> entries;
    size_t at = 0;
    while (at <= text.size() && entries.size() < max_entries) {
        const size_t nl = text.find('\n', at);
        const std::string_view line =
            nl == std::string_view::npos ? text.substr(at) : text.substr(at, nl - at);
        at = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        if (line.empty()) continue;
        const auto tok = split_ws(line);
        if (tok.size() < 4) continue;                       // header row and truncated lines
        ArpEntry e;
        if (!parse_ipv4(tok[0], &e.address)) continue;       // "IP address" header drops out here
        e.ip = tok[0];
        e.hw = tok[3];
        if (tok.size() >= 6) e.device = tok[5];
        camera::HardwareFingerprint fp;
        if (camera::HardwareFingerprint::normalize(tok[3], &fp, nullptr)) e.mac = fp.canonical();
        // Flags 0x0 means the kernel has not resolved this neighbour: not a device, an attempt.
        const std::string& flags = tok[2];
        const bool resolved = !(flags == "0x0" || flags == "0x00" || flags == "0");
        e.usable = resolved && !e.mac.empty();
        entries.push_back(std::move(e));
    }
    return entries;
}

// Candidates are drawn ONLY from ARP entries inside the configured host's own /24 whose MAC is
// exactly the recorded camera MAC. Nothing else can become a candidate.
inline std::optional<std::string> resolve_host_by_mac(const std::vector<ArpEntry>& entries,
                                                      const std::string& expected_mac,
                                                      uint32_t subnet_base) {
    camera::HardwareFingerprint want;
    if (!camera::HardwareFingerprint::normalize(expected_mac, &want, nullptr)) return std::nullopt;
    const std::string canonical = want.canonical();
    for (const auto& e : entries) {
        if (!e.usable) continue;
        if (e.mac != canonical) continue;
        if (!same_subnet24(e.address, subnet_base)) continue;
        return e.ip;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Injected effects. Note what is NOT here: a credential. Discovery cannot send one.
// ---------------------------------------------------------------------------------------------
// Diagnostic unauthenticated snapshot probe result. Never used as recovery identity.
struct SnapshotProbe {
    bool ok = false;          // a complete HTTP response was read
    int status = 0;
    std::string server;       // Server header, trimmed; bounded
    bool jpeg = false;        // SOI at body start AND EOI present
};

inline bool fingerprint_matches(const SnapshotProbe& probe, const std::string& expected_server) {
    if (!probe.ok || probe.status != 200 || !probe.jpeg) return false;
    if (expected_server.empty()) return true;   // JPEG alone; the caller re-verifies before LIVE
    if (probe.server.size() != expected_server.size()) return false;
    for (size_t i = 0; i < probe.server.size(); ++i) {
        const char a = static_cast<char>(::tolower(static_cast<unsigned char>(probe.server[i])));
        const char b = static_cast<char>(::tolower(static_cast<unsigned char>(expected_server[i])));
        if (a != b) return false;
    }
    return true;
}

struct LocatorIo {
    // Returns the TEXT of the ARP table. The default reads /proc/net/arp; tests supply fixtures.
    std::function<std::string()> read_arp;
    // Plain TCP connect to (ip, port). Sends NO bytes and reads NO bytes. True => connected.
    std::function<bool(const std::string& ip, uint16_t port, int timeout_ms)> tcp_probe;
    // Diagnostic unauthenticated snapshot fetch; discovery never calls this hook.
    std::function<SnapshotProbe(const std::string& ip, uint16_t port, const std::string& path,
                                int timeout_ms)>
        probe_snapshot;
    // Monotonic milliseconds. Used for both the rate limit and the scan deadline.
    std::function<int64_t()> now_ms;
};

// ---------------------------------------------------------------------------------------------
// Discovery outcome.
// ---------------------------------------------------------------------------------------------
enum class DiscoveryPhase { Idle, Searching, Found, NotFound };

inline const char* discovery_phase_name(DiscoveryPhase p) {
    switch (p) {
        case DiscoveryPhase::Searching: return "searching";
        case DiscoveryPhase::Found:     return "found";
        case DiscoveryPhase::NotFound:  return "not_found";
        case DiscoveryPhase::Idle:      break;
    }
    return "idle";
}

struct DiscoveryResult {
    bool found = false;
    bool rate_limited = false;     // refused by the 60 s gate; nothing was scanned
    bool scanned = false;          // a sweep actually ran
    bool fingerprint_used = false; // no MAC was known
    std::string host;              // set only when found
    std::string mac;               // the identity the candidate was matched on
    std::string reason;            // bounded, non-sensitive
    int scanned_hosts = 0;         // connects actually attempted
    int64_t started_ms = 0;
    int64_t duration_ms = 0;
};

// ---------------------------------------------------------------------------------------------
// The opaque state file: host + MAC + when the last scan ran. Written 0600, read bounded,
// refused when it is not owner-only (a world-writable file that seeds a host is a redirection
// primitive, so an insecure file is ignored rather than trusted).
// ---------------------------------------------------------------------------------------------
struct PersistedLocatorState {
    bool valid = false;
    std::string host;
    std::string mac;             // "" when the camera has never been resolved by MAC
    int64_t scanned_at_ms = 0;
};

inline bool load_locator_state(const std::string& path, PersistedLocatorState* out) {
    if (out == nullptr) return false;
    *out = PersistedLocatorState{};
    if (path.empty()) return false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    std::string text;
    bool ok = false;
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid() &&
        (st.st_mode & 077) == 0 && st.st_size >= 0 &&
        static_cast<size_t>(st.st_size) <= LocatorPolicy::kMaxStateBytes) {
        text.resize(static_cast<size_t>(st.st_size));
        size_t got = 0;
        while (got < text.size()) {
            const ssize_t n = ::read(fd, text.data() + got, text.size() - got);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        ok = (got == text.size());
        text.resize(got);
    }
    ::close(fd);
    if (!ok) return false;

    PersistedLocatorState parsed;
    bool magic_seen = false, host_seen = false;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = (nl == std::string::npos) ? text.size() : nl + 1;
        if (line.empty()) continue;
        if (!magic_seen) {
            if (line != "hgcam-locator 1") return false;   // unknown format: refuse, never guess
            magic_seen = true;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return false;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "host") {
            if (host_seen || !is_ipv4_literal(value)) return false;
            host_seen = true;
            parsed.host = value;
        } else if (key == "mac") {
            if (value.empty()) {
                parsed.mac.clear();
            } else {
                camera::HardwareFingerprint fp;
                if (!camera::HardwareFingerprint::normalize(value, &fp, nullptr)) return false;
                parsed.mac = fp.canonical();
            }
        } else if (key == "scanned_at_ms") {
            if (value.empty()) return false;
            int64_t v = 0;
            for (char c : value) {
                if (c < '0' || c > '9') return false;
                if (v > (INT64_MAX - (c - '0')) / 10) return false;
                v = v * 10 + (c - '0');
            }
            parsed.scanned_at_ms = v;
        } else {
            return false;                                   // unknown key: refuse
        }
    }
    if (!magic_seen || !host_seen) return false;
    parsed.valid = true;
    *out = std::move(parsed);
    return true;
}

inline bool store_locator_state(const std::string& path, const PersistedLocatorState& state) {
    if (path.empty() || !state.valid || !is_ipv4_literal(state.host)) return false;
    std::string mac;
    if (!state.mac.empty()) {
        camera::HardwareFingerprint fp;
        if (!camera::HardwareFingerprint::normalize(state.mac, &fp, nullptr)) return false;
        mac = fp.canonical();
    }
    std::string body = "hgcam-locator 1\nhost=" + state.host + "\nmac=" + mac +
                       "\nscanned_at_ms=" + std::to_string(state.scanned_at_ms) + "\n";
    if (body.size() > LocatorPolicy::kMaxStateBytes) return false;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    struct stat target {};
    if (::fstat(fd, &target) != 0 || !S_ISREG(target.st_mode) || target.st_uid != ::geteuid() ||
        target.st_nlink != 1 || ::ftruncate(fd, 0) != 0) { ::close(fd); return false; }
    // fchmod even on a brand-new file: O_CREAT honours the umask, and this file must be 0600
    // exactly - it names a device on the owner's network.
    if (::fchmod(fd, 0600) != 0) { ::close(fd); return false; }
    size_t written = 0;
    while (written < body.size()) {
        const ssize_t n = ::write(fd, body.data() + written, body.size() - written);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
    const bool complete = (written == body.size());
    ::close(fd);
    return complete;
}

// ---------------------------------------------------------------------------------------------
// Default (real) effects. Kept here so the pure algorithm below has no syscalls of its own.
// ---------------------------------------------------------------------------------------------
namespace locator_detail {

inline std::string read_proc_arp() {
    const int fd = ::open("/proc/net/arp", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string text;
    char buf[2048];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        if (text.size() + static_cast<size_t>(n) > LocatorPolicy::kMaxArpBytes) break;
        text.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return text;
}

// A bare connect: no payload, no auth, no read. This is the ONLY traffic a sweep produces.
inline bool tcp_connect_only(const std::string& ip, uint16_t port, int timeout_ms) {
    uint32_t address = 0;
    if (!parse_ipv4(ip, &address) || port == 0) return false;
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(address);
    bool connected = false;
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        connected = true;
    } else if (errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        const int ready = ::poll(&p, 1, timeout_ms < 1 ? 1 : timeout_ms);
        if (ready > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) connected = true;
        }
    }
    ::close(fd);   // close, never linger: no payload was ever written
    return connected;
}

inline bool valid_snapshot_path(const std::string& path) {
    if (path.empty() || path.size() > 256 || path.front() != '/') return false;
    for (char c : path) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\0') return false;
    }
    return true;
}

// One bounded, UNAUTHENTICATED GET. Only the status line, the Server header and the JPEG
// framing are examined; the body is not retained beyond the cap.
inline SnapshotProbe probe_snapshot(const std::string& ip, uint16_t port, const std::string& path,
                                    int timeout_ms) {
    SnapshotProbe out;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max(1, timeout_ms));
    auto remaining = [&] {
        return static_cast<int>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count()));
    };
    uint32_t address = 0;
    if (!parse_ipv4(ip, &address) || port == 0 || !valid_snapshot_path(path)) return out;
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return out;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return out;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(address);
    bool connected = false;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        connected = true;
    } else if (errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, remaining()) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) connected = true;
        }
    }
    if (!connected) {
        ::close(fd);
        return out;
    }
    auto ready = [&](short events) {
        const int left = remaining();
        if (left <= 0) return false;
        pollfd p{fd, events, 0};
        return ::poll(&p, 1, left) > 0;
    };

    // No Authorization header exists in this request, by design.
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + ip +
                                "\r\nAccept: image/jpeg\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size() && ready(POLLOUT)) {
        const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    std::string raw;
    if (sent == request.size()) {
        char buf[4096];
        while (raw.size() < 64u * 1024u && ready(POLLIN)) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
        }
    }
    ::close(fd);

    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos) return out;          // short read: no complete head
    const std::string head = raw.substr(0, head_end);
    const size_t line_end = head.find("\r\n");
    const std::string status_line = head.substr(0, line_end == std::string::npos ? head.size()
                                                                                : line_end);
    // "HTTP/1.1 200 OK"
    if (status_line.rfind("HTTP/1.", 0) != 0) return out;
    const size_t sp = status_line.find(' ');
    if (sp == std::string::npos || sp + 4 > status_line.size()) return out;
    out.status = (status_line[sp + 1] - '0') * 100 + (status_line[sp + 2] - '0') * 10 +
                 (status_line[sp + 3] - '0');
    // Server header, case-insensitive, bounded.
    size_t at = line_end == std::string::npos ? head.size() : line_end + 2;
    while (at < head.size()) {
        const size_t nl = head.find("\r\n", at);
        const std::string line = head.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? head.size() : nl + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (char& c : name) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (name != "server") continue;
        size_t v = colon + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
        out.server = line.substr(v, 128);
        break;
    }
    const std::string body = raw.substr(head_end + 4);
    out.jpeg = body.size() >= 4 && static_cast<unsigned char>(body[0]) == 0xFF &&
               static_cast<unsigned char>(body[1]) == 0xD8 &&
               body.find("\xFF\xD9") != std::string::npos;
    out.ok = true;
    return out;
}

inline int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline LocatorIo default_io() {
    LocatorIo io;
    io.read_arp = [] { return read_proc_arp(); };
    io.tcp_probe = [](const std::string& ip, uint16_t port, int timeout_ms) {
        return tcp_connect_only(ip, port, timeout_ms);
    };
    io.probe_snapshot = [](const std::string& ip, uint16_t port, const std::string& path,
                           int timeout_ms) { return probe_snapshot(ip, port, path, timeout_ms); };
    io.now_ms = [] { return steady_now_ms(); };
    return io;
}

}  // namespace locator_detail

// ---------------------------------------------------------------------------------------------
// The locator. Pure with respect to effects: every syscall it can reach is one of LocatorIo's.
// ---------------------------------------------------------------------------------------------
class CameraLocator {
public:
    CameraLocator(std::string configured_host, uint16_t port, std::string snapshot_path,
                  std::string expected_mac, LocatorIo io,
                  LocatorPolicy policy = LocatorPolicy())
        : configured_host_(std::move(configured_host)),
          port_(port),
          snapshot_path_(std::move(snapshot_path)),
          expected_mac_(std::move(expected_mac)),
          io_(std::move(io)),
          policy_(policy) {
        camera::HardwareFingerprint fp;
        if (camera::HardwareFingerprint::normalize(expected_mac_, &fp, nullptr))
            expected_mac_ = fp.canonical();
        else
            expected_mac_.clear();   // an unparseable MAC is "no MAC", never a wildcard
    }

    void set_authorization(std::function<bool()> authorize) { authorize_ = std::move(authorize); }
    bool destination_matches(const std::string& host) const {
        uint32_t configured = 0, destination = 0;
        if (expected_mac_.empty() || !parse_ipv4(configured_host_, &configured) ||
            !parse_ipv4(host, &destination) || !same_subnet24(configured, destination)) return false;
        bool matched = false;
        for (const auto& e : read_entries()) {
            if (e.ip != host) continue;
            if (!e.usable || e.mac != expected_mac_) return false;
            matched = true;
        }
        return matched;
    }
    const std::string& configured_host() const { return configured_host_; }
    const std::string& expected_mac() const { return expected_mac_; }
    const LocatorPolicy& policy() const { return policy_; }
    DiscoveryPhase phase() const { return phase_.load(); }
    int64_t last_scan_ms() const { return last_scan_ms_.load(); }
    int last_scanned_hosts() const { return last_scanned_hosts_.load(); }

    // Seed the rate limit and the last known host from a persisted state file, so a restart
    // inside the 60 s window does not rescan and a restart after it uses the known host first.
    //
    // The recorded time is on the monotonic clock, which restarts at boot. A seed is therefore
    // NOT Comparable with this process's clock: a value from a previous boot can be far larger
    // than today's uptime. It is marked as a seed and discarded (one scan) instead of being
    // mistaken for a rewound clock that would block rediscovery for hours.
    void seed_from_state(const PersistedLocatorState& state) {
        if (!state.valid) return;
        last_scan_ms_.store(state.scanned_at_ms);
        seeded_.store(state.scanned_at_ms != 0);
    }

    // Is this string a plausible camera host for a URL? No scheme, no port, no userinfo, no
    // path, no whitespace - so a discovered or supplied value can never rewrite the URL shape.
    static bool valid_host(const std::string& host) {
        if (host.empty() || host.size() > LocatorPolicy::kMaxHostChars) return false;
        for (char c : host) {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                            (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_';
            if (!ok) return false;
        }
        if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string::npos)
            return false;
        return true;
    }

    // Bounded rediscovery. Synchronous; at most one sweep per 60 s; never sends credentials.
    DiscoveryResult discover(int64_t now_ms) {
        DiscoveryResult r;
        if (authorize_ && !authorize_()) { r.reason = "source_unauthorized"; return r; }
        r.started_ms = now_ms;
        const int64_t start = now();
        r.duration_ms = 0;

        if (expected_mac_.empty()) {
            phase_.store(DiscoveryPhase::NotFound);
            r.reason = "explicit_mac_required";
            return r;
        }
        uint32_t configured_address = 0;
        if (!parse_ipv4(configured_host_, &configured_address)) {
            // A hostname cannot define a /24, and this module will not scan an arbitrary range.
            phase_.store(DiscoveryPhase::NotFound);
            r.reason = "configured_host_not_literal";
            return r;
        }
        const auto base = subnet24_of(configured_address);
        if (!base) {
            phase_.store(DiscoveryPhase::NotFound);
            r.reason = "configured_host_outside_site_network";
            return r;
        }

        // Rate limit FIRST, before any work: a failed discovery must not spin. `now_ms` is the
        // caller's clock for display; the injected clock decides the gate so tests can drive it.
        const int64_t gate_now = now();
        int64_t last = last_scan_ms_.load();
        if (last != 0) {
            if (gate_now < last && seeded_.load()) {
                // A seed from a previous boot: the monotonic clock restarted under it. Drop the
                // seed and scan once rather than refuse for as long as the old value exceeds
                // today's uptime.
                seeded_.store(false);
                last_scan_ms_.store(0);
                last = 0;
            }
            if (last != 0 && gate_now < last) {           // clock rewound: fail closed, no sweep
                phase_.store(DiscoveryPhase::NotFound);
                r.reason = "clock_went_backward";
                return r;
            }
            if (last != 0 && gate_now - last < policy_.min_scan_interval_ms) {
                r.rate_limited = true;
                r.reason = "rate_limited";
                r.duration_ms = now() - start;
                return r;
            }
        }
        seeded_.store(false);
        // Recorded BEFORE the sweep: a crash mid-scan still counts as an attempt.
        last_scan_ms_.store(gate_now);
        phase_.store(DiscoveryPhase::Searching);
        const int64_t deadline = gate_now + std::min(policy_.overall_deadline_ms, LocatorPolicy::kOverallDeadlineMs);

        std::vector<ArpEntry> entries = read_entries();
        int scanned = 0;
        std::optional<std::string> host;

        if (!expected_mac_.empty()) {
            // PRIMARY: the recorded MAC. Identity, resolved through the kernel's neighbour table.
            host = resolve_host_by_mac(entries, expected_mac_, *base);
            if (!host) {
                // Prime the ARP table with plain connects to the camera's own port, then re-read.
                scanned = sweep(*base, deadline);
                entries = read_entries();
                host = resolve_host_by_mac(entries, expected_mac_, *base);
            }
            if (!host) r.reason = "mac_not_found_on_subnet";
        }

        r.scanned = true;
        r.scanned_hosts = scanned;
        last_scanned_hosts_.store(scanned);
        if (host) {
            r.found = true;
            r.host = *host;
            r.mac = expected_mac_;
            r.reason = "identified";
            phase_.store(DiscoveryPhase::Found);
        } else {
            phase_.store(DiscoveryPhase::NotFound);
        }
        r.duration_ms = now() - start;
        return r;
    }

    // Diagnostic metadata only. Discovery never uses this as identity.
    void set_expected_server(std::string server) { expected_server_ = std::move(server); }
    const std::string& expected_server() const { return expected_server_; }

private:
    int64_t now() const { return io_.now_ms ? io_.now_ms() : 0; }

    std::vector<ArpEntry> read_entries() const {
        if (!io_.read_arp) return {};
        std::string text = io_.read_arp();
        if (text.size() > policy_.max_arp_bytes) text.resize(policy_.max_arp_bytes);
        return parse_arp(text, policy_.max_arp_entries);
    }

    std::vector<ArpEntry> candidates(const std::vector<ArpEntry>& entries,
                                     uint32_t base) const {
        std::vector<ArpEntry> out;
        for (const auto& e : entries) {
            if (!e.usable) continue;
            if (!same_subnet24(e.address, base)) continue;   // no other subnet, ever
            out.push_back(e);
        }
        return out;
    }

    // Sweep the configured /24 with bare connects. Waves of at most `max_concurrent_probes`,
    // each wave joined before the next, so the concurrency bound is structural rather than
    // statistical. The deadline is checked before each wave and inside each worker.
    //
    // Thread creation can fail (EAGAIN under load). That must not escape into the HTTP handler and
    // take the portal down for a camera problem, so a failed wave is joined and the sweep stops:
    // a partial sweep is a smaller failure than a crash.
    int sweep(uint32_t base, int64_t deadline) const {
        if (!io_.tcp_probe) return 0;
        int attempted = 0;
        const uint32_t network = base;
        for (int offset = 1; offset <= 254 && attempted < policy_.max_hosts;) {
            if (now() >= deadline || (authorize_ && !authorize_())) break;
            const size_t remaining = static_cast<size_t>(policy_.max_hosts - attempted);
            const size_t wave = std::min<size_t>(
                static_cast<size_t>(std::clamp(policy_.max_concurrent_probes, 1, LocatorPolicy::kMaxConcurrentProbes)), remaining);
            std::vector<std::thread> workers;
            size_t launched = 0;
            try {
                workers.reserve(wave);
                for (size_t i = 0; i < wave; ++i) {
                    const int host_offset = offset + static_cast<int>(i);
                    if (host_offset > 254) break;
                    const std::string ip = format_ipv4(network | static_cast<uint32_t>(host_offset));
                    workers.emplace_back([this, ip, deadline] {
                        if (authorize_ && !authorize_()) return;
                        const int64_t left = deadline - now();
                        if (left <= 0) return;
                        (void)io_.tcp_probe(ip, port_, static_cast<int>(std::min<int64_t>(
                            left, std::min(policy_.probe_timeout_ms, LocatorPolicy::kProbeTimeoutMs))));
                    });
                    ++launched;
                }
            } catch (const std::exception&) {
                // fall through with whatever was launched: join it, then stop.
            }
            for (auto& t : workers) {
                if (t.joinable()) t.join();
            }
            attempted += static_cast<int>(launched);
            if (launched < wave) break;   // the wave failed part-way: do not keep trying
            offset += static_cast<int>(launched);
        }
        return attempted;
    }

    std::string configured_host_;
    uint16_t port_ = 80;
    std::string snapshot_path_ = "/snapshot";
    std::string expected_mac_;
    std::string expected_server_;
    LocatorIo io_;
    std::function<bool()> authorize_;
    LocatorPolicy policy_;
    std::atomic<DiscoveryPhase> phase_{DiscoveryPhase::Idle};
    std::atomic<int64_t> last_scan_ms_{0};
    std::atomic<int> last_scanned_hosts_{0};
    std::atomic<bool> seeded_{false};   // last_scan_ms_ came from a state file, not this process
};

}  // namespace portal
}  // namespace homeguardian

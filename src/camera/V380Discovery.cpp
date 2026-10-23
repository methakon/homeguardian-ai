#include "camera/V380Discovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace homeguardian {
namespace v380 {
namespace {

constexpr char kQuery[] = "NVDEVSEARCH^100";
constexpr char kResultPrefix[] = "NVDEVRESULT";
constexpr size_t kMinFields = 13;

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool looks_like_mac(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i % 3 == 2) {
            if (s[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

bool looks_like_ipv4(const std::string& s) {
    struct in_addr a{};
    return ::inet_pton(AF_INET, s.c_str(), &a) == 1;
}

std::vector<std::string> split_fields(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : text) {
        if (c == '^') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r' && c != '\n' && c != '\0') {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

std::string discovery_query() { return kQuery; }

DiscoveryRecord parse_discovery_response(const std::string& text) {
    DiscoveryRecord r;
    const auto f = split_fields(text);
    if (f.size() < kMinFields) {
        r.reason = "fewer than the documented field count: not a discovery response";
        return r;
    }
    if (f[0] != kResultPrefix) {
        r.reason = "does not start with the documented result prefix";
        return r;
    }
    const std::string mac = lowered(f[2]);
    if (!looks_like_mac(mac)) {
        r.reason = "field 2 is not a MAC address";
        return r;
    }
    if (!looks_like_ipv4(f[3])) {
        r.reason = "field 3 is not an IPv4 address";
        return r;
    }
    // The device id is a decimal integer in the field the confirmed reading reproduced exactly. A record
    // without a plausible id cannot become an endpoint: it is refused here rather than resolved later.
    int32_t device_id = 0;
    try {
        size_t consumed = 0;
        device_id = std::stoi(f[12], &consumed);
        if (consumed != f[12].size()) throw std::invalid_argument("trailing characters");
    } catch (const std::exception&) {
        r.reason = "field 12 is not a decimal device id";
        return r;
    }
    if (device_id <= 0) {
        r.reason = "device id must be positive";
        return r;
    }

    r.valid = true;
    r.mac = mac;
    r.address = f[3];
    r.netmask = f[4];
    r.gateway = f[5];
    r.device_id = device_id;
    // The device advertises several ports; the vendor media port is the one this codec speaks to.
    for (size_t i = 6; i < f.size() && i <= 11; ++i) {
        if (f[i] == "8800") { r.media_port = 8800; break; }
    }
    return r;
}

UdpV380Discovery::UdpV380Discovery() = default;
UdpV380Discovery::UdpV380Discovery(Config config) : cfg_(std::move(config)) {}

std::vector<DiscoveryRecord> UdpV380Discovery::discover(std::chrono::milliseconds timeout) {
    std::vector<DiscoveryRecord> found;
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return found;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(cfg_.reply_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        ::close(fd);
        return found;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg_.query_port);
    if (::inet_pton(AF_INET, cfg_.broadcast_address.c_str(), &dst.sin_addr) != 1) {
        ::close(fd);
        return found;
    }
    const std::string q = discovery_query();
    ::sendto(fd, q.data(), q.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) break;
        timeval tv{static_cast<time_t>(remaining / 1000000),
                   static_cast<suseconds_t>(remaining % 1000000)};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[2048] = {0};
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) break;
        const auto record = parse_discovery_response(std::string(buf, static_cast<size_t>(n)));
        if (record.valid) found.push_back(record);
    }
    ::close(fd);
    return found;
}

}  // namespace v380
}  // namespace homeguardian

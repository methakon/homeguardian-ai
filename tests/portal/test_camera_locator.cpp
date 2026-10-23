#include <catch2/catch.hpp>

#include "bridge/CameraLocator.h"
#include "bridge/LiveCameraFeed.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Everything here runs OFFLINE: the ARP table is synthetic text, the connect function is a
// lambda that records what it was asked to do instead of opening a socket, and the only real
// sockets are HTTP peers bound to 127.0.0.0/8. No real camera and no real LAN is touched, and
// no discovery probe can carry a credential (CameraLocator has no parameter for one).

using namespace homeguardian;
using namespace homeguardian::portal;

namespace {

const char* kCameraMac = "aa:bb:cc:dd:ee:01";

std::vector<uint8_t> fake_jpeg() {
    return {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
            0xFF, 0xD9};
}

std::string arp_line(const std::string& ip, const std::string& mac,
                     const std::string& flags = "0x2", const std::string& device = "eth0") {
    return ip + std::string(12, ' ') + "0x1          " + flags + std::string(10, ' ') + mac +
           std::string(15, ' ') + "*        " + device + "\n";
}

std::string arp_table(const std::vector<std::pair<std::string, std::string>>& rows) {
    std::string text =
        "IP address       HW type     Flags       HW address            Mask     Device\n";
    for (const auto& r : rows) text += arp_line(r.first, r.second);
    return text;
}

// A deterministic, thread-safe stand-in for every effect the locator can reach.
struct OfflineIo {
    std::string arp_text;
    std::atomic<int64_t> now{1000000};
    int probe_delay_ms = 0;      // makes overlapping probes observable
    int probe_ticks = 0;         // how much virtual time each probe consumes
    std::atomic<int> live{0};
    std::atomic<int> peak{0};
    mutable std::mutex mu;
    std::vector<std::string> probed_ips;
    std::vector<uint16_t> probed_ports;
    int fingerprint_calls = 0;
    std::function<SnapshotProbe(const std::string&, uint16_t, const std::string&)>
        fingerprint = [](const std::string&, uint16_t, const std::string&) {
            return SnapshotProbe{};
        };

    LocatorIo io() {
        LocatorIo out;
        out.read_arp = [this] { return arp_text; };
        out.tcp_probe = [this](const std::string& ip, uint16_t port, int) {
            const int concurrent = live.fetch_add(1) + 1;
            int prev = peak.load();
            while (concurrent > prev && !peak.compare_exchange_weak(prev, concurrent)) {
            }
            {
                std::lock_guard<std::mutex> lock(mu);
                probed_ips.push_back(ip);
                probed_ports.push_back(port);
            }
            if (probe_delay_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(probe_delay_ms));
            if (probe_ticks > 0) now.fetch_add(probe_ticks);
            live.fetch_sub(1);
            return false;   // "nothing answered": identity comes from ARP, never from a connect
        };
        out.probe_snapshot = [this](const std::string& ip, uint16_t port, const std::string& path,
                                    int) {
            {
                std::lock_guard<std::mutex> lock(mu);
                ++fingerprint_calls;
            }
            return fingerprint(ip, port, path);
        };
        out.now_ms = [this] { return now.load(); };
        return out;
    }

    int probed_count() const {
        std::lock_guard<std::mutex> lock(mu);
        return static_cast<int>(probed_ips.size());
    }
    std::vector<std::string> probed() const {
        std::lock_guard<std::mutex> lock(mu);
        return probed_ips;
    }
    std::vector<uint16_t> ports() const {
        std::lock_guard<std::mutex> lock(mu);
        return probed_ports;
    }
};

// A minimal loopback HTTP peer. It records the request it was given (so a test can prove that
// NO Authorization header ever arrived) and answers with whatever the responder returned.
class TestHttpPeer {
public:
    using Responder = std::function<std::string(const std::string& request)>;

    TestHttpPeer(const std::string& bind_ip, Responder responder)
        : responder_(std::move(responder)) {
        listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(listener_ >= 0);
        int one = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        ::inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listener_, 8) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~TestHttpPeer() {
        serving_.store(false);
        if (worker_.joinable()) worker_.join();   // exits on the next 50 ms poll tick
        ::close(listener_);
    }

    uint16_t port() const { return port_; }

    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }

private:
    void serve() {
        for (;;) {
            if (!serving_.load()) return;
            pollfd p{listener_, POLLIN, 0};
            const int ready = ::poll(&p, 1, 50);
            if (ready <= 0) continue;
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) {
                if (!serving_.load()) return;
                continue;
            }
            timeval tv{1, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string request;
            char buf[1024];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
                if (n <= 0) break;
                request.append(buf, static_cast<size_t>(n));
                if (request.size() > 8192) break;
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                requests_.push_back(request);
            }
            const std::string reply = responder_(request);
            size_t sent = 0;
            while (sent < reply.size()) {
                const ssize_t n = ::send(client, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
            }
            ::close(client);
        }
    }

    int listener_ = -1;
    uint16_t port_ = 0;
    Responder responder_;
    std::thread worker_;
    mutable std::mutex mu_;
    std::vector<std::string> requests_;
    std::atomic<bool> serving_{true};
};

std::string http_response(int status, const std::string& server, const std::string& body,
                          const char* reason = "OK") {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nServer: " + server + "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

std::string jpeg_body() {
    const auto bytes = fake_jpeg();
    return std::string(bytes.begin(), bytes.end());
}

std::string temp_dir() {
    const std::string dir =
        std::filesystem::temp_directory_path().string() + "/hg_camera_locator_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("ARP table parsing handles /proc/net/arp including incomplete entries",
          "[camera-locator][camera]") {
    const std::string text =
        "IP address       HW type     Flags       HW address            Mask     Device\n" +
        arp_line("10.0.0.2", "aa:bb:cc:00:00:02") +
        arp_line("10.0.0.3", "00:00:00:00:00:00", "0x0") +      // unresolved neighbour
        arp_line("10.0.0.4", "00:00:00:00:00:00", "0x2") +      // all-zero HW address
        arp_line("10.0.0.5", "AA:BB:CC:00:00:05", "0x2", "wlan0") +   // upper case
        "not a table row\n" +
        "10.0.0.6         0x1  0x2  00:11:22:33:44\n";          // truncated MAC

    const auto entries = parse_arp(text);
    REQUIRE(entries.size() == 5);

    REQUIRE(entries[0].usable);
    REQUIRE(entries[0].mac == "aa:bb:cc:00:00:02");
    REQUIRE(entries[0].device == "eth0");

    // An incomplete entry is an attempt, not a device: both spellings are refused.
    REQUIRE_FALSE(entries[1].usable);
    REQUIRE(entries[1].mac.empty());
    REQUIRE_FALSE(entries[2].usable);

    // Normalized to the same canonical form as the configured MAC (so a match is a match).
    REQUIRE(entries[3].mac == "aa:bb:cc:00:00:05");
    REQUIRE(entries[3].device == "wlan0");

    // A malformed hardware address is kept for diagnosis but is never a candidate.
    REQUIRE(entries[4].ip == "10.0.0.6");
    REQUIRE_FALSE(entries[4].usable);

    // The entry cap is enforced, not advisory.
    REQUIRE(parse_arp(text, 2).size() == 2);
    REQUIRE(parse_arp("").empty());
    REQUIRE(parse_arp("IP address HW type Flags HW address Mask Device\n").empty());
}

TEST_CASE("A recorded MAC resolves the camera's new host with no subnet traffic",
          "[camera-locator][camera]") {
    OfflineIo io;
    // The camera moved from .10 (now held by another device) to .42.
    io.arp_text = arp_table({{"10.0.0.10", "aa:bb:cc:dd:ee:99"},
                             {"10.0.0.42", kCameraMac}});
    CameraLocator locator("10.0.0.10", 80, "/snapshot", kCameraMac, io.io());

    const auto r = locator.discover(io.now.load());
    REQUIRE(r.found);
    REQUIRE(r.host == "10.0.0.42");
    REQUIRE(r.mac == kCameraMac);
    REQUIRE(r.reason == "identified");
    // Identity came from the kernel's table, so not one connect was needed.
    REQUIRE(r.scanned_hosts == 0);
    REQUIRE(io.probed_count() == 0);
    REQUIRE(locator.phase() == DiscoveryPhase::Found);
    REQUIRE(locator.last_scanned_hosts() == 0);
}

TEST_CASE("A candidate with a different MAC is rejected and nothing is swept if the MAC is present",
          "[camera-locator][camera]") {
    OfflineIo io;
    // Only a different device is on the subnet: the camera is genuinely gone.
    io.arp_text = arp_table({{"10.0.0.10", "aa:bb:cc:dd:ee:99"}});
    CameraLocator locator("10.0.0.10", 8554, "/snapshot", kCameraMac, io.io());

    const auto r = locator.discover(io.now.load());
    REQUIRE_FALSE(r.found);
    REQUIRE(r.reason == "mac_not_found_on_subnet");
    REQUIRE(locator.phase() == DiscoveryPhase::NotFound);

    // The one entry that IS present must never be handed to the caller as the camera.
    REQUIRE(r.host.empty());

    // A sweep ran, but it is bounded to the configured /24, to the camera's port, and it is
    // connects only: no payload, no HTTP, no credential.
    REQUIRE(r.scanned_hosts == 254);
    REQUIRE(io.probed_count() == 254);
    for (const auto& ip : io.probed()) {
        REQUIRE(ip.rfind("10.0.0.", 0) == 0);
        REQUIRE(ip != "10.0.0.0");
        REQUIRE(ip != "10.0.0.255");
    }
    for (uint16_t port : io.ports()) REQUIRE(port == 8554);
}

TEST_CASE("No MAC known refuses discovery without any probe",
          "[camera-locator][camera]") {
    OfflineIo io;
    io.arp_text = arp_table({{"10.0.0.30", "aa:bb:cc:00:00:30"}});
    // A 200 that is not a JPEG is an auth page or an error document, not a camera frame.
    io.fingerprint = [](const std::string&, uint16_t, const std::string&) {
        SnapshotProbe p;
        p.ok = true;
        p.status = 200;
        p.server = "Venus";
        p.jpeg = false;
        return p;
    };
    CameraLocator locator("10.0.0.10", 80, "/snapshot", "", io.io());
    locator.set_expected_server("Venus");

    const auto r = locator.discover(io.now.load());
    REQUIRE_FALSE(r.found);
    REQUIRE(r.reason == "explicit_mac_required");
    REQUIRE_FALSE(r.fingerprint_used);
    REQUIRE(io.fingerprint_calls == 0);
    REQUIRE(io.probed_count() == 0);
}

TEST_CASE("A sweep is capped at 254 hosts and 16 concurrent connects",
          "[camera-locator][camera]") {
    OfflineIo io;
    io.arp_text.clear();          // nothing resolved yet: the sweep is what primes the table
    io.probe_delay_ms = 3;        // long enough that the waves really overlap
    CameraLocator locator("10.0.9.10", 80, "/snapshot", kCameraMac, io.io());

    const auto r = locator.discover(io.now.load());
    REQUIRE_FALSE(r.found);
    REQUIRE(r.scanned_hosts == 254);
    REQUIRE(io.probed_count() == 254);

    const auto ips = io.probed();
    std::set<std::string> unique(ips.begin(), ips.end());
    REQUIRE(unique.size() == 254);
    REQUIRE(io.peak.load() >= 2);                    // the test really did overlap
    REQUIRE(io.peak.load() <= LocatorPolicy::kMaxConcurrentProbes);
    REQUIRE(io.peak.load() <= 16);
    for (const auto& ip : unique) REQUIRE(ip.rfind("10.0.9.", 0) == 0);
}

TEST_CASE("Only the configured host's own /24 is ever probed", "[camera-locator][camera]") {
    OfflineIo io;
    CameraLocator locator("10.7.3.44", 80, "/snapshot", kCameraMac, io.io());
    const auto r = locator.discover(io.now.load());
    REQUIRE_FALSE(r.found);
    REQUIRE(io.probed_count() == 254);
    for (const auto& ip : io.probed()) {
        REQUIRE(ip.rfind("10.7.3.", 0) == 0);        // never .0 and never another subnet
    }
    // A configured host that is not an IPv4 literal cannot define a /24, so nothing is scanned
    // rather than a range being invented.
    OfflineIo other;
    CameraLocator hostname_locator("camera.local", 80, "/snapshot", kCameraMac, other.io());
    const auto r2 = hostname_locator.discover(other.now.load());
    REQUIRE_FALSE(r2.found);
    REQUIRE(r2.reason == "configured_host_not_literal");
    REQUIRE(other.probed_count() == 0);
}

TEST_CASE("The overall deadline stops a sweep early instead of running to the cap",
          "[camera-locator][camera]") {
    OfflineIo io;
    io.probe_ticks = 300;         // each probe burns 300 ms of the 5 s budget
    LocatorPolicy policy;
    policy.overall_deadline_ms = 5000;
    CameraLocator locator("10.0.1.10", 80, "/snapshot", kCameraMac, io.io(), policy);

    const int64_t start = io.now.load();
    const auto r = locator.discover(start);
    REQUIRE_FALSE(r.found);
    REQUIRE(r.scanned_hosts < 254);              // it stopped instead of running to the cap
    REQUIRE(r.scanned_hosts <= 64);
    REQUIRE(io.now.load() - start >= 4000);      // it did spend the budget, then stopped
    // Each probe is bounded by the per-host timeout, so the overrun is at most one in-flight
    // wave rather than open-ended.
    REQUIRE(r.duration_ms <= policy.overall_deadline_ms +
                                 policy.max_concurrent_probes * io.probe_ticks + 500);
    // The attempt is recorded, so the next one is a minute away rather than immediate.
    REQUIRE(locator.last_scan_ms() == start);
}

TEST_CASE("Rediscovery is limited to once per 60 s and a failure cannot spin",
          "[camera-locator][camera]") {
    OfflineIo io;
    io.arp_text = arp_table({{"10.0.0.10", "aa:bb:cc:dd:ee:99"}});
    CameraLocator locator("10.0.0.10", 80, "/snapshot", kCameraMac, io.io());

    const int64_t t0 = io.now.load();
    const auto first = locator.discover(t0);
    REQUIRE_FALSE(first.found);
    const int after_first = io.probed_count();
    REQUIRE(after_first == 254);

    // 59 s later: refused, and NOT ONE connect is made. A failed discovery does not become a loop.
    io.now.store(t0 + 59000);
    const auto second = locator.discover(t0 + 59000);
    REQUIRE(second.rate_limited);
    REQUIRE(second.reason == "rate_limited");
    REQUIRE_FALSE(second.scanned);
    REQUIRE(second.scanned_hosts == 0);
    REQUIRE(io.probed_count() == after_first);

    // At 60 s the gate opens again.
    io.now.store(t0 + 60000);
    const auto third = locator.discover(t0 + 60000);
    REQUIRE_FALSE(third.rate_limited);
    REQUIRE(io.probed_count() == after_first + 254);

    // A clock that jumps BACKWARD from the recorded scan is refused rather than read as "a minute
    // has passed" (a rewound clock would otherwise allow a sweep on every call).
    io.now.store(t0 + 1000);
    const auto rewound = locator.discover(t0 + 1000);
    REQUIRE_FALSE(rewound.scanned);
    REQUIRE(rewound.reason == "clock_went_backward");
    REQUIRE(io.probed_count() == after_first + 254);

    // A seed recorded on a PREVIOUS BOOT is not a rewound clock: the monotonic clock restarted
    // under it. It must not block rediscovery, so the seed is dropped and one scan runs.
    OfflineIo rebooted;
    rebooted.arp_text = arp_table({{"10.0.0.10", "aa:bb:cc:dd:ee:99"}});
    CameraLocator fresh("10.0.0.10", 80, "/snapshot", kCameraMac, rebooted.io());
    PersistedLocatorState seed;
    seed.valid = true;
    seed.host = "10.0.0.42";
    seed.mac = kCameraMac;
    seed.scanned_at_ms = rebooted.now.load() + 3600000;   // "before the reboot": far in the future
    fresh.seed_from_state(seed);
    const auto after_reboot = fresh.discover(rebooted.now.load());
    REQUIRE_FALSE(after_reboot.rate_limited);
    REQUIRE(after_reboot.scanned);
    REQUIRE(rebooted.probed_count() == 254);

    // And a seed from a restart moments ago DOES suppress the rescan.
    OfflineIo restarted;
    restarted.arp_text = arp_table({{"10.0.0.10", "aa:bb:cc:dd:ee:99"}});
    CameraLocator resumed("10.0.0.10", 80, "/snapshot", kCameraMac, restarted.io());
    PersistedLocatorState recent;
    recent.valid = true;
    recent.host = "10.0.0.42";
    recent.mac = kCameraMac;
    recent.scanned_at_ms = restarted.now.load() - 15000;   // 15 s ago
    resumed.seed_from_state(recent);
    const auto too_soon = resumed.discover(restarted.now.load());
    REQUIRE(too_soon.rate_limited);
    REQUIRE(restarted.probed_count() == 0);
}

TEST_CASE("The discovery state file round-trips at 0600, is bounded, and is refused if insecure",
          "[camera-locator][camera]") {
    const std::string path = temp_dir() + "/locator_state";
    ::unlink(path.c_str());

    PersistedLocatorState written;
    written.valid = true;
    written.host = "10.0.0.42";
    written.mac = "AA:BB:CC:DD:EE:01";     // normalized on the way in
    written.scanned_at_ms = 424242;
    REQUIRE(store_locator_state(path, written));

    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == 0600);
    REQUIRE(st.st_uid == ::geteuid());
    REQUIRE(static_cast<size_t>(st.st_size) <= LocatorPolicy::kMaxStateBytes);

    PersistedLocatorState read_back;
    REQUIRE(load_locator_state(path, &read_back));
    REQUIRE(read_back.valid);
    REQUIRE(read_back.host == "10.0.0.42");
    REQUIRE(read_back.mac == "aa:bb:cc:dd:ee:01");
    REQUIRE(read_back.scanned_at_ms == 424242);

    // A file that is not owner-only could be written by another account to redirect the portal,
    // so it is ignored rather than trusted.
    REQUIRE(::chmod(path.c_str(), 0644) == 0);
    PersistedLocatorState insecure;
    REQUIRE_FALSE(load_locator_state(path, &insecure));

    // Garbage, an unknown format and an over-long file are all refused.
    ::unlink(path.c_str());
    {
        std::ofstream f(path, std::ios::binary);
        f << "host=10.0.0.42\n";
    }
    REQUIRE(::chmod(path.c_str(), 0600) == 0);
    REQUIRE_FALSE(load_locator_state(path, &insecure));

    ::unlink(path.c_str());
    {
        std::ofstream f(path, std::ios::binary);
        f << "hgcam-locator 1\nhost=10.0.0.42\nmac=aa:bb:cc:dd:ee:01\nscanned_at_ms=5\n";
        f << std::string(600, 'x');
    }
    REQUIRE(::chmod(path.c_str(), 0600) == 0);
    REQUIRE_FALSE(load_locator_state(path, &insecure));

    // And a state naming something that is not an address is not written in the first place.
    PersistedLocatorState bogus;
    bogus.valid = true;
    bogus.host = "http://evil/snapshot";
    bogus.scanned_at_ms = 1;
    REQUIRE_FALSE(store_locator_state(temp_dir() + "/locator_state_bogus", bogus));
    REQUIRE_FALSE(load_locator_state("", &insecure));
}

TEST_CASE("Default probes: a closed port fails fast, an open port connects, a short read never "
          "matches a fingerprint",
          "[camera-locator][camera]") {
    // An open loopback port accepts a bare connect; a closed one refuses immediately.
    TestHttpPeer peer("127.0.0.1", [](const std::string&) { return std::string(); });
    REQUIRE(locator_detail::tcp_connect_only("127.0.0.1", peer.port(), 220));

    // A connect to an address that is not there must fail promptly, not hang for the timeout.
    uint16_t closed_port = 0;
    {
        TestHttpPeer transient_peer("127.0.0.1", [](const std::string&) { return std::string(); });
        closed_port = transient_peer.port();
    }
    const auto connect_start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(locator_detail::tcp_connect_only("127.0.0.1", closed_port, 220));
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - connect_start)
                                .count();
    REQUIRE(connect_ms < 300);   // refused, not waited out

    SECTION("a truncated response is not a camera") {
        TestHttpPeer truncated("127.0.0.1", [](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nServer: Ven");   // dies mid-header
        });
        const auto probe =
            locator_detail::probe_snapshot("127.0.0.1", truncated.port(), "/snapshot", 400);
        REQUIRE_FALSE(probe.ok);
        REQUIRE_FALSE(fingerprint_matches(probe, "Venus"));
    }

    SECTION("a 200 that is not a JPEG is not a camera") {
        TestHttpPeer html("127.0.0.1", [](const std::string&) {
            return http_response(200, "Venus", "<html>login</html>");
        });
        const auto probe = locator_detail::probe_snapshot("127.0.0.1", html.port(), "/snapshot", 400);
        REQUIRE(probe.ok);
        REQUIRE(probe.status == 200);
        REQUIRE(probe.server == "Venus");
        REQUIRE_FALSE(probe.jpeg);
        REQUIRE_FALSE(fingerprint_matches(probe, "Venus"));
    }

    SECTION("server header and JPEG framing identify the device, case-insensitively") {
        TestHttpPeer camera("127.0.0.1", [](const std::string&) {
            return http_response(200, "Venus/1.0", jpeg_body());
        });
        const auto probe =
            locator_detail::probe_snapshot("127.0.0.1", camera.port(), "/snapshot", 400);
        REQUIRE(probe.ok);
        REQUIRE(probe.status == 200);
        REQUIRE(probe.server == "Venus/1.0");
        REQUIRE(probe.jpeg);
        REQUIRE(fingerprint_matches(probe, "venus/1.0"));   // case-insensitive
        REQUIRE_FALSE(fingerprint_matches(probe, "nginx")); // a different product
        // The probe request carries no credential of any kind.
        const auto requests = camera.requests();
        REQUIRE(requests.size() == 1);
        REQUIRE(requests[0].rfind("GET /snapshot HTTP/1.1", 0) == 0);
        REQUIRE_FALSE(has(requests[0], "Authorization"));
        REQUIRE_FALSE(has(requests[0], "Basic"));
    }

    SECTION("an unreachable host produces no probe at all") {
        const auto probe = locator_detail::probe_snapshot("127.0.0.1", closed_port, "/snapshot", 1);
        REQUIRE_FALSE(probe.ok);
        REQUIRE_FALSE(fingerprint_matches(probe, ""));
    }
}

TEST_CASE("Matching JPEG decoys cannot become recovery candidates without an explicit MAC",
          "[camera-locator][camera]") {
    OfflineIo io;
    io.arp_text = arp_table({{"127.0.0.2", kCameraMac}});
    io.fingerprint = [](const std::string&, uint16_t, const std::string&) {
        return SnapshotProbe{true, 200, "Venus", true};
    };
    CameraLocator locator("127.0.0.1", 80, "/snapshot", "", io.io());
    const auto r = locator.discover(io.now.load());
    REQUIRE_FALSE(r.found);
    REQUIRE(r.reason == "explicit_mac_required");
    REQUIRE(io.fingerprint_calls == 0);
    REQUIRE(io.probed_count() == 0);
}

TEST_CASE("A verified rediscovery moves the feed to the new host and clears DISCONNECTED",
          "[camera-locator][camera]") {
    // The camera now answers on 127.0.0.2; the configured address 127.0.0.1 has nothing listening.
    TestHttpPeer camera("127.0.0.2", [](const std::string& request) {
        if (!has(request, "Authorization: Basic ")) return std::string();   // must be authenticated
        return http_response(200, "Venus/1.0", jpeg_body());
    });

    const std::string state_path = temp_dir() + "/feed_state_ok";
    ::unlink(state_path.c_str());

    CameraConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = camera.port();
    cfg.snapshot_path = "/snapshot";
    cfg.user = "admin";
    cfg.password = "correct-horse-battery";
    cfg.allow_plaintext = true;
    cfg.mac = kCameraMac;
    cfg.scan_enabled = true;
    cfg.state_file = state_path;
    LiveCameraFeed feed(cfg);

    auto io = std::make_shared<OfflineIo>();
    io->arp_text = arp_table({{"127.0.0.1", "aa:bb:cc:dd:ee:99"},
                              {"127.0.0.2", kCameraMac}});
    io->now.store(2000);
    feed.set_locator(std::make_shared<CameraLocator>(cfg.host, cfg.port, cfg.snapshot_path,
                                                    cfg.mac, io->io()));

    // Three failed fetches: the portal reports DISCONNECTED honestly.
    REQUIRE_FALSE(feed.fetch_frame(1000).ok);
    REQUIRE_FALSE(feed.fetch_frame(1300).ok);
    REQUIRE_FALSE(feed.fetch_frame(1600).ok);
    REQUIRE(feed.status(1600).state == FeedState::Disconnected);

    const auto r = feed.rediscover(2000);
    REQUIRE(r.attempted);
    REQUIRE(r.found);
    REQUIRE(r.verified);
    REQUIRE(r.host == "127.0.0.2");
    REQUIRE(feed.config().host == "127.0.0.2");

    const auto st = feed.status(2000);
    REQUIRE(st.state == FeedState::Live);
    REQUIRE(st.discovery_state == "found");
    REQUIRE(st.discovery_source == "rediscovered");
    REQUIRE(st.discovery_reason.empty());
    REQUIRE(st.frames_served >= 1);

    // The camera really did receive the credential, and only it did.
    const auto requests = camera.requests();
    REQUIRE(requests.size() >= 1);
    REQUIRE(has(requests[0], "Authorization: Basic "));

    // The state file records the verified host at 0600, so a restart does not rescan.
    PersistedLocatorState saved;
    REQUIRE(load_locator_state(state_path, &saved));
    REQUIRE(saved.host == "127.0.0.2");
    REQUIRE(saved.mac == kCameraMac);
    REQUIRE(saved.scanned_at_ms == 2000);

    // The JSON keeps every existing key and adds the discovery block (no address, no MAC,
    // no credential).
    const std::string json = feed.status_json(2000);
    REQUIRE(has(json, "\"provenance\":\"physical_camera\""));
    REQUIRE(has(json, "\"delivery\":\"refreshing_snapshot\""));
    REQUIRE(has(json, "\"is_continuous_video\":false"));
    REQUIRE(has(json, "\"analysis_connected\":false"));
    REQUIRE(has(json, "\"freshness_ms\":"));
    REQUIRE(has(json, "\"discovery\":{\"state\":\"found\""));
    REQUIRE(has(json, "\"source\":\"rediscovered\""));
    REQUIRE(has(json, "\"enabled\":true"));
    REQUIRE(has(json, "\"scanned_hosts\":0"));
    REQUIRE_FALSE(has(json, cfg.password));
    REQUIRE_FALSE(has(json, "Authorization"));
    REQUIRE_FALSE(has(json, "127.0.0.2"));   // the address is not published to the panel
}

TEST_CASE("A failed re-verification keeps the last known host and reports not_found",
          "[camera-locator][camera]") {
    // The candidate IS at the new address, but rejects the credential: identity is not enough.
    TestHttpPeer impostor("127.0.0.2", [](const std::string&) {
        return std::string("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
    });

    CameraConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = impostor.port();
    cfg.snapshot_path = "/snapshot";
    cfg.user = "admin";
    cfg.password = "wrong";
    cfg.allow_plaintext = true;
    cfg.mac = kCameraMac;
    cfg.scan_enabled = true;
    LiveCameraFeed feed(cfg);

    auto io = std::make_shared<OfflineIo>();
    io->arp_text = arp_table({{"127.0.0.2", kCameraMac}});
    io->now.store(2000);
    feed.set_locator(std::make_shared<CameraLocator>(cfg.host, cfg.port, cfg.snapshot_path,
                                                    cfg.mac, io->io()));

    REQUIRE_FALSE(feed.fetch_frame(1000).ok);
    REQUIRE_FALSE(feed.fetch_frame(1300).ok);
    REQUIRE_FALSE(feed.fetch_frame(1600).ok);
    REQUIRE(feed.status(1600).state == FeedState::Disconnected);

    const auto r = feed.rediscover(2000);
    REQUIRE(r.attempted);
    REQUIRE(r.found);            // identity matched ...
    REQUIRE_FALSE(r.verified);   // ... but the frame did not
    REQUIRE(r.reason == "reverify_failed");

    // The feed did not move to an unverified address, and it says so rather than pretending.
    REQUIRE(feed.config().host == "127.0.0.1");
    const auto st = feed.status(2000);
    REQUIRE(st.discovery_state == "not_found");
    REQUIRE(st.discovery_reason == "reverify_failed");
    REQUIRE(st.discovery_source == "configured");
    REQUIRE(st.state == FeedState::Disconnected);
    REQUIRE(feed.status_json(2000).find("127.0.0.2") == std::string::npos);
}

TEST_CASE("Discovery never sends the camera credential to a host that is not the camera",
          "[camera-locator][camera]") {
    // A live HTTP peer sits at the configured address and would happily record credentials. The
    // camera is NOT on this subnet (its MAC is in no ARP entry), so nothing may reach that peer.
    TestHttpPeer decoy("127.0.0.1", [](const std::string&) {
        return http_response(200, "Venus/1.0", jpeg_body());
    });

    CameraConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = decoy.port();
    cfg.snapshot_path = "/snapshot";
    cfg.user = "admin";
    cfg.password = "must-never-leave";
    cfg.allow_plaintext = true;
    cfg.mac = kCameraMac;
    cfg.scan_enabled = true;
    LiveCameraFeed feed(cfg);

    auto io = std::make_shared<OfflineIo>();
    io->arp_text = arp_table({{"127.0.0.1", "aa:bb:cc:dd:ee:99"},
                              {"127.0.0.3", "aa:bb:cc:dd:ee:98"}});
    io->now.store(5000);
    feed.set_locator(std::make_shared<CameraLocator>(cfg.host, cfg.port, cfg.snapshot_path,
                                                    cfg.mac, io->io()));

    const auto r = feed.rediscover(5000);
    REQUIRE_FALSE(r.found);
    REQUIRE(r.reason == "mac_not_found_on_subnet");
    REQUIRE(r.scanned_hosts == 254);       // bounded work, not an unbounded hunt
    REQUIRE(io->probed_count() == 254);
    REQUIRE(io->fingerprint_calls == 0);   // MAC mode never falls back to HTTP probing

    // The decisive assertion: the decoy received NOTHING - not a request line, not a header.
    REQUIRE(decoy.requests().empty());
    REQUIRE(feed.config().host == "127.0.0.1");
    REQUIRE(feed.status(5000).discovery_state == "not_found");

    // A second attempt inside the minute is refused outright: a missing camera cannot spin.
    const auto again = feed.rediscover(5500);
    REQUIRE(again.rate_limited);
    REQUIRE(io->probed_count() == 254);
    REQUIRE(decoy.requests().empty());
}

TEST_CASE("An opted-out feed never scans, and an untouched feed does not sweep the LAN",
          "[camera-locator][camera]") {
    CameraConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 9;                 // nothing listens; the fetch fails, which is the point
    cfg.allow_plaintext = true;
    cfg.mac = kCameraMac;
    cfg.scan_enabled = false;     // the default
    LiveCameraFeed feed(cfg);

    REQUIRE_FALSE(feed.maybe_rediscover(1000));
    REQUIRE_FALSE(feed.fetch_frame(1000).ok);
    REQUIRE_FALSE(feed.maybe_rediscover(1200));   // still off, even with a failure recorded

    const std::string json = feed.status_json(1000);
    REQUIRE(has(json, "\"state\":\"CONNECTING\""));
    REQUIRE(has(json, "\"discovery\":{\"state\":\"idle\""));
    REQUIRE(has(json, "\"enabled\":false"));
    REQUIRE(has(json, "\"source\":\"configured\""));
    REQUIRE(has(json, "\"last_scan_ms\":0"));

    // Scan enabled but nothing has failed yet: a startup status poll must NOT sweep the LAN.
    CameraConfig armed = cfg;
    armed.scan_enabled = true;
    LiveCameraFeed ready(armed);
    auto io = std::make_shared<OfflineIo>();
    io->now.store(1000);
    ready.set_locator(std::make_shared<CameraLocator>(armed.host, armed.port,
                                                     armed.snapshot_path, armed.mac, io->io()));
    REQUIRE_FALSE(ready.maybe_rediscover(1000));
    REQUIRE(io->probed_count() == 0);
    REQUIRE(ready.status(1000).state == FeedState::NeverConnected);

    // One failed fetch is enough evidence; the next poll attempts exactly one bounded discovery.
    REQUIRE_FALSE(ready.fetch_frame(1000).ok);
    REQUIRE(ready.status(1000).consecutive_failures == 1);
    (void)ready.maybe_rediscover(1300);
    REQUIRE(io->probed_count() == 254);
}

TEST_CASE("A host value is validated before it can enter the camera URL",
          "[camera-locator][camera]") {
    REQUIRE(CameraLocator::valid_host("10.0.0.42"));
    REQUIRE(CameraLocator::valid_host("camera.local"));
    REQUIRE_FALSE(CameraLocator::valid_host(""));
    REQUIRE_FALSE(CameraLocator::valid_host("http://10.0.0.42"));
    REQUIRE_FALSE(CameraLocator::valid_host("10.0.0.42:80"));
    REQUIRE_FALSE(CameraLocator::valid_host("admin:pw@10.0.0.42"));
    REQUIRE_FALSE(CameraLocator::valid_host("10.0.0.42/snapshot"));
    REQUIRE_FALSE(CameraLocator::valid_host("10.0.0.42\r\nX: y"));
    REQUIRE_FALSE(CameraLocator::valid_host(std::string(80, 'a')));

    CameraConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.allow_plaintext = true;
    LiveCameraFeed feed(cfg);
    REQUIRE_FALSE(feed.set_host("http://evil.example/snapshot"));
    REQUIRE_FALSE(feed.set_host(""));
    REQUIRE(feed.config().host == "127.0.0.1");
    REQUIRE(feed.set_host("10.0.0.42"));
    REQUIRE(feed.config().host == "10.0.0.42");
}

TEST_CASE("Normal auto recovery fetch refuses wrong or unknown destination MAC before HTTP",
          "[camera-locator][camera]") {
    TestHttpPeer decoy("127.0.0.1", [](const std::string&) {
        return http_response(200, "Venus", jpeg_body());
    });
    CameraConfig cfg;
    cfg.host = "127.0.0.1"; cfg.port = decoy.port(); cfg.mac = kCameraMac;
    cfg.allow_plaintext = true; cfg.scan_enabled = true;
    LiveCameraFeed feed(cfg);
    OfflineIo io;
    io.arp_text = arp_table({{"127.0.0.1", "aa:bb:cc:dd:ee:99"}});
    feed.set_locator(std::make_shared<CameraLocator>(cfg.host, cfg.port, cfg.snapshot_path,
                                                   cfg.mac, io.io()));
    REQUIRE_FALSE(feed.fetch_frame(1000).ok);
    REQUIRE(decoy.requests().empty());
    io.arp_text.clear();
    REQUIRE_FALSE(feed.fetch_frame(2000).ok);
    REQUIRE(decoy.requests().empty());
}

TEST_CASE("Saved host is never adopted at construction even when matching MAC or scan off",
          "[camera-locator][camera]") {
    const auto path = temp_dir() + "/untrusted_hint";
    PersistedLocatorState state;
    state.valid = true; state.host = "127.0.0.2"; state.mac = kCameraMac;
    state.scanned_at_ms = 1000;
    REQUIRE(store_locator_state(path, state));
    CameraConfig cfg;
    cfg.host = "127.0.0.1"; cfg.mac = kCameraMac; cfg.state_file = path;
    SECTION("off") { cfg.scan_enabled = false; }
    SECTION("auto") { cfg.scan_enabled = true; }
    LiveCameraFeed feed(cfg);
    REQUIRE(feed.config().host == "127.0.0.1");
}

TEST_CASE("Authorization withdrawal during a sweep stops subsequent network probes", "[camera-locator][camera][security]") {
    OfflineIo fixture;
    auto io = fixture.io();
    std::atomic<bool> allowed{true};
    int probes = 0;
    io.tcp_probe = [&](const std::string&, uint16_t, int) { ++probes; allowed = false; return false; };
    LocatorPolicy policy;
    policy.max_concurrent_probes = 1;
    CameraLocator locator("127.0.0.1", 80, "/snapshot", kCameraMac, io, policy);
    locator.set_authorization([&] { return allowed.load(); });
    REQUIRE_FALSE(locator.discover(fixture.now.load()).found);
    REQUIRE(probes == 1);
}

TEST_CASE("Failed scan cooldown persists across feed reconstruction without adopting a host", "[camera-locator][camera][security]") {
    CameraConfig cfg;
    cfg.host = "127.0.0.1"; cfg.mac = kCameraMac; cfg.scan_enabled = true;
    cfg.state_file = temp_dir() + "/failed_scan_cooldown";
    ::unlink(cfg.state_file.c_str());
    OfflineIo fixture;
    fixture.now = locator_detail::steady_now_ms();
    LiveCameraFeed feed(cfg);
    feed.set_locator(std::make_shared<CameraLocator>(cfg.host, 80, cfg.snapshot_path, cfg.mac, fixture.io()));
    REQUIRE_FALSE(feed.rediscover(fixture.now.load()).verified);
    PersistedLocatorState state;
    REQUIRE(load_locator_state(cfg.state_file, &state));
    REQUIRE(state.scanned_at_ms == fixture.now.load());
    LiveCameraFeed restarted(cfg);
    fixture.probed_ips.clear();
    restarted.set_locator(std::make_shared<CameraLocator>(cfg.host, 80, cfg.snapshot_path, cfg.mac, fixture.io()));
    REQUIRE(restarted.rediscover(fixture.now.load()).rate_limited);
    REQUIRE(fixture.probed_count() == 0);
    REQUIRE(restarted.config().host == cfg.host);
    ::unlink(cfg.state_file.c_str());
}

TEST_CASE("Destination MAC changed after initial check suppresses all credential bytes", "[camera-locator][camera][security]") {
    TestHttpPeer peer("127.0.0.1", [](const std::string&) { return std::string(); });
    CameraConfig cfg;
    cfg.host = "127.0.0.1"; cfg.port = peer.port(); cfg.mac = kCameraMac;
    cfg.allow_plaintext = true; cfg.scan_enabled = true;
    LiveCameraFeed feed(cfg);
    OfflineIo fixture;
    auto io = fixture.io();
    int reads = 0;
    io.read_arp = [&] { return arp_table({{cfg.host, ++reads == 1 ? kCameraMac : "aa:bb:cc:dd:ee:99"}}); };
    feed.set_locator(std::make_shared<CameraLocator>(cfg.host, cfg.port, cfg.snapshot_path, cfg.mac, io));
    REQUIRE_FALSE(feed.fetch_frame(1000).ok);
    REQUIRE(reads >= 2);
    // A connection can have been accepted, but zero request/credential bytes may arrive.
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    for (const auto& request : peer.requests()) REQUIRE(request.empty());
}

TEST_CASE("Scan probes receive only the remaining absolute deadline budget", "[camera-locator][camera][security]") {
    OfflineIo fixture;
    auto io = fixture.io();
    int supplied = 0;
    io.tcp_probe = [&](const std::string&, uint16_t, int timeout) {
        supplied = timeout;
        fixture.now.fetch_add(timeout);
        return false;
    };
    LocatorPolicy policy;
    policy.max_concurrent_probes = 1;
    policy.overall_deadline_ms = 35;
    CameraLocator locator("127.0.0.1", 80, "/snapshot", kCameraMac, io, policy);
    locator.discover(fixture.now.load());
    REQUIRE(supplied == 35);
    REQUIRE(fixture.now.load() == 1000035);
}

TEST_CASE("Capture and registry denial prevent all recovery and credential work", "[camera-locator][camera][security]") {
    CameraConfig cfg;
    cfg.host = "127.0.0.1"; cfg.mac = kCameraMac;
    cfg.scan_enabled = true; cfg.allow_plaintext = true;
    LiveCameraFeed feed(cfg);
    OfflineIo fixture;
    int reads = 0;
    auto io = fixture.io();
    io.read_arp = [&] { ++reads; return std::string(); };
    feed.set_locator(std::make_shared<CameraLocator>(cfg.host, 80, cfg.snapshot_path, cfg.mac, io));
    // Record a failure before withdrawing authorization, like the status polling regression.
    feed.fetch_frame(1000);
    reads = 0;
    feed.set_authorization([] { return false; });
    REQUIRE_FALSE(feed.maybe_rediscover(2000));
    REQUIRE_FALSE(feed.rediscover(3000).attempted);
    REQUIRE_FALSE(feed.fetch_frame(4000).ok);
    REQUIRE(reads == 0);
    REQUIRE(fixture.probed_count() == 0);
}

TEST_CASE("Authorization revoked after candidate discovery prevents credential fetch", "[camera-locator][camera][security]") {
    TestHttpPeer peer("127.0.0.2", [](const std::string&) { return http_response(200, "Venus", jpeg_body()); });
    CameraConfig cfg;
    cfg.host = "127.0.0.1"; cfg.port = peer.port(); cfg.mac = kCameraMac;
    cfg.scan_enabled = true; cfg.allow_plaintext = true;
    LiveCameraFeed feed(cfg);
    bool allowed = true;
    feed.set_authorization([&] { return allowed; });
    OfflineIo fixture;
    auto io = fixture.io();
    io.read_arp = [&] { allowed = false; return arp_table({{"127.0.0.2", kCameraMac}}); };
    feed.set_locator(std::make_shared<CameraLocator>(cfg.host, cfg.port, cfg.snapshot_path, cfg.mac, io));
    REQUIRE_FALSE(feed.rediscover(2000).verified);
    REQUIRE(peer.requests().empty());
    REQUIRE(feed.config().host == cfg.host);
}

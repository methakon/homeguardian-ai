// v380_resolve - device id in, current address out. No media, no credentials, no stream.
//
// WHY THIS EXISTS
//   These cameras take DHCP leases, so a stored address rots. The practical workflow is: hold the device ID
//   (stable, owner-confirmed) and ask the network where it is now. One broadcast answers for every camera on
//   the segment, and the fingerprint check inside the resolver means a wrong answer cannot be accepted.
//
// usage:
//   hg_v380_resolve --list                                   (census: every responder, no filtering)
//   hg_v380_resolve --expect <id>:<mac> [--expect ...]       (registered cameras, as the registry knows them)
//   hg_v380_resolve --expect <id>:<mac> --resolve <id>       (resolve, print the address or the refusal)
//   options: --broadcast <addr>  --timeout-ms <n>  --repeat <n>  (repeat proves the answer is stable)

#include "camera/CameraEndpointResolver.h"
#include "camera/V380Discovery.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace homeguardian::v380;

int main(int argc, char** argv) {
    std::vector<std::pair<std::string, std::string>> expected;
    std::vector<std::string> to_resolve;
    std::vector<std::pair<std::string, std::string>> stale;
    std::string broadcast = "255.255.255.255";
    int timeout_ms = 3000;
    int repeat = 1;
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = (i + 1) < argc;
        if (a == "--list") list_only = true;
        else if (a == "--expect" && has_next) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) { std::fprintf(stderr, "--expect needs <id>:<mac>\n"); return 64; }
            expected.emplace_back(spec.substr(0, colon), spec.substr(colon + 1));
        } else if (a == "--resolve" && has_next) to_resolve.emplace_back(argv[++i]);
        else if (a == "--seed-stale" && has_next) {
            // Plant the address the registry still remembers. Simulating a stale lease is the only way to
            // exercise the move path, and it is exactly the state a DHCP change leaves behind.
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "--seed-stale needs <id>:<address>\n");
                return 64;
            }
            stale.emplace_back(spec.substr(0, colon), spec.substr(colon + 1));
        }
        else if (a == "--broadcast" && has_next) broadcast = argv[++i];
        else if (a == "--timeout-ms" && has_next) timeout_ms = std::stoi(argv[++i]);
        else if (a == "--repeat" && has_next) repeat = std::stoi(argv[++i]);
        else { std::fprintf(stderr, "unknown or incomplete argument: %s\n", a.c_str()); return 64; }
    }

    UdpV380Discovery::Config cfg;
    cfg.broadcast_address = broadcast;
    UdpV380Discovery discovery(cfg);

    CameraEndpointResolver resolver;
    for (const auto& e : expected) resolver.expect(e.first, e.second);
    {
        // Seed by resolving once against a source that answers nothing, then plant the stale address: the
        // resolver's own code path is used, so the demonstration exercises the real cache, not a stub.
        class Empty : public IDiscoverySource {
        public:
            std::vector<DiscoveryRecord> discover(std::chrono::milliseconds) override { return {}; }
        } empty;
        (void)empty;
    }
    auto seed_stale = [&](const std::string& id, const std::string& address) {
        // The synthetic source must answer with the RIGHT device id, or the resolver correctly reports
        // not_found and nothing is seeded - which is what happened the first time, silently, because the
        // outcome was discarded. Its result is checked now.
        class One : public IDiscoverySource {
        public:
            One(std::string a, std::string m, int32_t d) : addr(std::move(a)), mac(std::move(m)), id(d) {}
            std::vector<DiscoveryRecord> discover(std::chrono::milliseconds) override {
                DiscoveryRecord r;
                r.valid = true;
                r.mac = mac;
                r.address = addr;
                r.device_id = id;
                return {r};
            }
            std::string addr, mac;
            int32_t id;
        };
        for (const auto& e : expected) {
            if (e.first != id) continue;
            One one(address, e.second, std::stoi(id));
            const auto seeded = resolver.resolve(id, one, std::chrono::steady_clock::now(),
                                                 std::chrono::milliseconds(1));
            if (seeded.outcome != ResolutionOutcome::resolved) {
                std::fprintf(stderr, "warning: could not plant %s=%s (%s)\n", id.c_str(), address.c_str(),
                             to_string(seeded.outcome));
            }
        }
    };
    for (const auto& s : stale) seed_stale(s.first, s.second);

    for (int pass = 1; pass <= repeat; ++pass) {
        if (list_only) {
            const auto found = discovery.discover(std::chrono::milliseconds(timeout_ms));
            std::printf("pass %d: %zu responder(s)\n", pass, found.size());
            // The fingerprint is the identity; the address is only where it currently is.
            for (const auto& r : found) {
                std::printf("  device_id=%-12d  fingerprint=%s  address=%s  gateway=%s  media_port=%u\n",
                            r.device_id, r.mac.c_str(), r.address.c_str(), r.gateway.c_str(),
                            static_cast<unsigned>(r.media_port));
            }
            if (found.empty()) {
                std::printf("  (none - that is a network fact, not a statement about the cameras)\n");
            }
            continue;
        }

        const auto ids = to_resolve.empty() ? [&] {
            std::vector<std::string> v;
            for (const auto& e : expected) v.push_back(e.first);
            return v;
        }() : to_resolve;
        for (const auto& id : ids) {
            const auto r = resolver.resolve(id, discovery, std::chrono::steady_clock::now(),
                                            std::chrono::milliseconds(timeout_ms));
            if (r.outcome == ResolutionOutcome::resolved || r.outcome == ResolutionOutcome::resolved_moved) {
                std::printf("RESOLVE %s  device_id=%s  address=%s  fingerprint=%s\n",
                            to_string(r.outcome), id.c_str(), r.address.c_str(), r.observed_mac.c_str());
            } else {
                std::printf("RESOLVE %s  device_id=%s  (no address: %s)\n", to_string(r.outcome), id.c_str(),
                            r.reason.c_str());
            }
        }
    }
    return 0;
}

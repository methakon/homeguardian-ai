// test_endpoint_resolver: how a logical camera becomes an address, and every way that must be refused.
//
// SYNTHETIC VALUES ONLY. Synthetic fingerprints (00:11:22:33:44:xx), a neutral example range (10.20.30.x) and
// placeholder device ids: the repository is public and a real address, MAC or device identifier does not
// belong in it. The parse test is still a real validation of the field mapping - that mapping was confirmed
// against a live device separately, and the values here carry none of it.

#include <catch2/catch.hpp>

#include "camera/CameraEndpointResolver.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian::v380;

namespace {

std::string result_line(const std::string& mac, const std::string& ip, const std::string& id) {
    return "NVDEVRESULT^200^" + mac + "^" + ip + "^255.255.255.0^10.20.30.1^5040^5050^5051^8800^7050^5040^" +
           id + "^0";
}

// A stub discovery source: the resolver's rules are tested with no network and no cameras at all.
class StubDiscovery : public IDiscoverySource {
public:
    explicit StubDiscovery(std::vector<std::string> lines) : lines_(std::move(lines)) {}
    std::vector<DiscoveryRecord> discover(std::chrono::milliseconds) override {
        ++calls;
        std::vector<DiscoveryRecord> out;
        for (const auto& l : lines_) {
            const auto r = parse_discovery_response(l);
            if (r.valid) out.push_back(r);
        }
        return out;
    }
    std::vector<std::string> lines_;
    int calls = 0;
};

using clock_t_ = std::chrono::steady_clock;

}  // namespace

TEST_CASE("discovery: a well-formed response becomes a record", "[resolve][parse]") {
    const auto r = parse_discovery_response(result_line("00:11:22:33:44:01", "10.20.30.40", "12000001"));
    INFO(r.reason);
    REQUIRE(r.valid);
    REQUIRE(r.mac == "00:11:22:33:44:01");
    REQUIRE(r.address == "10.20.30.40");
    REQUIRE(r.device_id == 12000001);
    REQUIRE(r.media_port == 8800);
    // An UPPERCASE MAC is the same device, so it is normalized rather than refused.
    REQUIRE(parse_discovery_response(result_line("00:11:22:33:44:AA", "10.20.30.40", "12000001")).mac ==
            "00:11:22:33:44:aa");
}

TEST_CASE("discovery: malformed responses are refused, never half-parsed", "[resolve][parse][refusal]") {
    const std::string good = result_line("00:11:22:33:44:01", "10.20.30.40", "12000001");
    REQUIRE_FALSE(parse_discovery_response("").valid);
    REQUIRE_FALSE(parse_discovery_response("HELLO^200^x").valid);
    REQUIRE_FALSE(parse_discovery_response(good.substr(0, 40)).valid);           // truncated
    REQUIRE_FALSE(parse_discovery_response(result_line("not-a-mac", "10.20.30.40", "12000001")).valid);
    REQUIRE_FALSE(parse_discovery_response(result_line("00:11:22:33:44:01", "999.1.1.1", "12000001")).valid);
    REQUIRE_FALSE(parse_discovery_response(result_line("00:11:22:33:44:01", "10.20.30.40", "abc")).valid);
    REQUIRE_FALSE(parse_discovery_response(result_line("00:11:22:33:44:01", "10.20.30.40", "0")).valid);
    REQUIRE_FALSE(parse_discovery_response(result_line("00:11:22:33:44:01", "10.20.30.40", "-5")).valid);
    REQUIRE_FALSE(parse_discovery_response(result_line("00:11:22:33:44:01", "10.20.30.40", "12x")).valid);
}

TEST_CASE("resolver: a registered camera resolves, and a DHCP move is not a new device",
          "[resolve][identity]") {
    CameraEndpointResolver resolver;
    resolver.expect("12000001", "00:11:22:33:44:01");

    StubDiscovery first({result_line("00:11:22:33:44:01", "10.20.30.40", "12000001")});
    const auto now = clock_t_::now();
    const auto a = resolver.resolve("12000001", first, now, std::chrono::milliseconds(1));
    REQUIRE(a.outcome == ResolutionOutcome::resolved);
    REQUIRE(a.address == "10.20.30.40");
    REQUIRE(resolver.resolutions() == 1);
    REQUIRE(resolver.moves_detected() == 0);

    // The camera takes a new lease. Same fingerprint, same device id, different address.
    StubDiscovery second({result_line("00:11:22:33:44:01", "10.20.30.77", "12000001")});
    const auto b = resolver.resolve("12000001", second, now + std::chrono::seconds(1),
                                    std::chrono::milliseconds(1));
    REQUIRE(b.outcome == ResolutionOutcome::resolved_moved);
    REQUIRE(b.address == "10.20.30.77");
    REQUIRE(resolver.moves_detected() == 1);
    REQUIRE(b.reason.find("identity did not") != std::string::npos);
}

TEST_CASE("resolver: the fingerprint decides, so a rogue id-claimer is refused", "[resolve][security]") {
    CameraEndpointResolver resolver;
    resolver.expect("12000001", "00:11:22:33:44:01");
    // Another device presents the SAME device id with a DIFFERENT fingerprint. Accepting it would let it
    // inherit this camera's identity and consent.
    StubDiscovery rogue({result_line("00:11:22:33:44:99", "10.20.30.40", "12000001")});
    const auto r = resolver.resolve("12000001", rogue, clock_t_::now(), std::chrono::milliseconds(1));
    REQUIRE(r.outcome == ResolutionOutcome::fingerprint_mismatch);
    REQUIRE(r.address.empty());          // fail closed: nothing to connect to
    REQUIRE(resolver.refusals() == 1);
    REQUIRE(r.reason.find("consent") != std::string::npos);
}

TEST_CASE("resolver: the expected fingerprint under a different device id is reported as a mismatch",
          "[resolve][identity]") {
    CameraEndpointResolver resolver;
    resolver.expect("12000001", "00:11:22:33:44:01");
    StubDiscovery stranger({result_line("00:11:22:33:44:01", "10.20.30.40", "12000099")});
    const auto r = resolver.resolve("12000001", stranger, clock_t_::now(), std::chrono::milliseconds(1));
    REQUIRE(r.outcome == ResolutionOutcome::fingerprint_mismatch);
    REQUIRE(r.address.empty());
}

TEST_CASE("resolver: contradictory observations apply NOTHING", "[resolve][conflict]") {
    SECTION("two responders claim one device id") {
        CameraEndpointResolver resolver;
        resolver.expect("12000001", "00:11:22:33:44:01");
        StubDiscovery dup({result_line("00:11:22:33:44:01", "10.20.30.40", "12000001"),
                           result_line("00:11:22:33:44:01", "10.20.30.41", "12000001")});
        const auto r = resolver.resolve("12000001", dup, clock_t_::now(), std::chrono::milliseconds(1));
        REQUIRE(r.outcome == ResolutionOutcome::conflict);
        REQUIRE(r.address.empty());
        REQUIRE(resolver.refusals() == 1);
    }
    SECTION("two responders claim one address") {
        CameraEndpointResolver resolver;
        resolver.expect("12000001", "00:11:22:33:44:01");
        StubDiscovery clash({result_line("00:11:22:33:44:01", "10.20.30.40", "12000001"),
                             result_line("00:11:22:33:44:02", "10.20.30.40", "12000002")});
        const auto r = resolver.resolve("12000001", clash, clock_t_::now(), std::chrono::milliseconds(1));
        REQUIRE(r.outcome == ResolutionOutcome::conflict);
        REQUIRE(r.address.empty());
    }
}

TEST_CASE("resolver: absence is reported as absence, and an unregistered id is not onboarded",
          "[resolve][refusal]") {
    CameraEndpointResolver resolver;
    resolver.expect("12000001", "00:11:22:33:44:01");
    StubDiscovery nothing({});
    REQUIRE(resolver.resolve("12000001", nothing, clock_t_::now(), std::chrono::milliseconds(1)).outcome ==
            ResolutionOutcome::not_found);

    // A camera nobody registered must never be resolved into existence - that is onboarding, by accident.
    StubDiscovery present({result_line("00:11:22:33:44:05", "10.20.30.50", "12000005")});
    const auto r = resolver.resolve("12000005", present, clock_t_::now(), std::chrono::milliseconds(1));
    REQUIRE(r.outcome == ResolutionOutcome::not_found);
    REQUIRE(r.reason.find("not registered") != std::string::npos);
    REQUIRE(resolver.refusals() == 1);
}

TEST_CASE("resolver: the cache is short-lived and one failed connection invalidates it",
          "[resolve][cache]") {
    CameraEndpointResolver resolver(CameraEndpointResolver::Config{std::chrono::seconds(120)});
    resolver.expect("12000001", "00:11:22:33:44:01");
    StubDiscovery stub({result_line("00:11:22:33:44:01", "10.20.30.40", "12000001")});
    const auto now = clock_t_::now();
    REQUIRE(resolver.resolve("12000001", stub, now, std::chrono::milliseconds(1)).outcome ==
            ResolutionOutcome::resolved);

    std::string addr;
    REQUIRE(resolver.cached_address("12000001", now + std::chrono::seconds(30), &addr));
    REQUIRE(addr == "10.20.30.40");
    // Past the TTL the device is asked again rather than trusted.
    REQUIRE_FALSE(resolver.cached_address("12000001", now + std::chrono::seconds(121), &addr));
    // A failed connection makes the address suspect immediately, before any TTL expires.
    resolver.invalidate("12000001");
    REQUIRE_FALSE(resolver.cached_address("12000001", now, &addr));
    // And the fast path never invents an address for an unknown camera.
    REQUIRE_FALSE(resolver.cached_address("12000099", now, &addr));
}

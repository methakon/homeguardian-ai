// test_v380_adapter: the V380 EC39 adapter, held to the measurements rather than to hopes about the device.
//
// The adapter's most important behaviour is that it REFUSES: no RTSP or ONVIF interface was measured on this
// camera in its default state, so code that "connected" today would be inventing a protocol. These tests pin
// that refusal, the identity rule (an address is not an identity), consent gating with purge, and the
// bounded media path.

#include <catch2/catch.hpp>

#include "backend/linux/V380SourceAdapter.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

V380SourceAdapter::Config config(const std::string& identity = "hg-v380-ec39-01",
                                 const std::string& host = "192.168.1.19") {
    V380SourceAdapter::Config cfg;
    cfg.enrolled_identity = identity;
    cfg.endpoint_host = host;
    cfg.consent_granted = true;
    return cfg;
}

}  // namespace

TEST_CASE("v380: the capabilities are the measured ones, and the unverified ones say so",
          "[v380][capabilities]") {
    V380SourceAdapter adapter(config());
    const auto& caps = adapter.capabilities();
    INFO(caps.evidence);
    // MEASURED absences: an adapter that assumed otherwise would be inventing protocol support.
    REQUIRE_FALSE(caps.rtsp_listener_present);
    REQUIRE_FALSE(caps.onvif_listener_present);
    REQUIRE_FALSE(caps.local_interface_available);
    // DOCUMENTED but not verified here, and labelled as such.
    REQUIRE(caps.rtsp_documentedly_enableable);
    REQUIRE_FALSE(caps.verified_by_this_worker);
    REQUIRE(caps.documented_video_codec == "H.264");
    // NOT VERIFIED: audio and talkback are never assumed, and talkback is never silently enabled.
    REQUIRE_FALSE(caps.microphone_verified);
    REQUIRE_FALSE(caps.talkback_verified);
}

TEST_CASE("v380: it refuses with a code and an owner action instead of guessing a protocol",
          "[v380][refusal]") {
    V380SourceAdapter adapter(config());
    std::string error;
    const auto refusal = adapter.open(&error);
    REQUIRE(refusal == V380Refusal::no_verified_local_interface);
    REQUIRE(v380_refusal_to_string(refusal) == std::string("no_verified_local_interface"));
    REQUIRE(error.find("OWNER ACTION") != std::string::npos);
    REQUIRE(error.find("RTSP or ONVIF") != std::string::npos);
    REQUIRE(error.find("re-verify") != std::string::npos);

    MediaFrame frame;
    V380Refusal read_refusal = V380Refusal::ok;
    std::string why;
    REQUIRE_FALSE(adapter.read_frame(&frame, &read_refusal, &why));
    REQUIRE(read_refusal == V380Refusal::not_opened);
}

TEST_CASE("v380: an address is not an identity", "[v380][identity]") {
    SECTION("no enrolled identity is refused even with consent and a reachable address") {
        auto cfg = config(/*identity=*/"", /*host=*/"192.168.1.19");
        V380SourceAdapter adapter(cfg);
        std::string error;
        REQUIRE(adapter.open(&error) == V380Refusal::identity_unverified);
        REQUIRE(error.find("never be its identity") != std::string::npos);
    }
    SECTION("a DHCP change does not change the logical source") {
        // docs/HG_V380_07_NETWORK_CHANGE_REDISCOVERY.md records this exact problem: the camera moved from
        // 192.168.0.164 to 192.168.1.20. The identity must survive that.
        auto cfg = config();
        cfg.synthetic_fixture = true;                 // so a frame path exists without the device
        V380SourceAdapter adapter(cfg);
        std::string error;
        REQUIRE(adapter.open(&error) == V380Refusal::ok);
        const std::string before = adapter.identity();

        adapter.update_endpoint("192.168.1.42");      // a new lease
        REQUIRE(adapter.identity() == before);        // unchanged: identity is the enrolled id
        MediaFrame frame;
        REQUIRE(adapter.read_frame(&frame, nullptr, nullptr));
    }
}

TEST_CASE("v380: consent gates capture and withdrawal purges what was buffered", "[v380][consent]") {
    auto cfg = config();
    cfg.synthetic_fixture = true;
    cfg.buffer.max_frames = 8;
    V380SourceAdapter adapter(cfg);

    std::string error;
    REQUIRE(adapter.open(&error) == V380Refusal::ok);
    MediaFrame frame;
    for (int i = 0; i < 3; ++i) REQUIRE(adapter.read_frame(&frame, nullptr, nullptr));

    const size_t discarded = adapter.withdraw_consent("owner revoked consent for this camera");
    REQUIRE(discarded >= 0);
    REQUIRE(adapter.buffered_frames() == 0);
    // After withdrawal nothing more is produced, and it is a consent refusal - not a silent stop.
    V380Refusal refusal = V380Refusal::ok;
    std::string why;
    REQUIRE_FALSE(adapter.read_frame(&frame, &refusal, &why));
    REQUIRE(refusal == V380Refusal::consent_withheld);
}

TEST_CASE("v380: capture without consent is refused rather than queued", "[v380][consent][refusal]") {
    auto cfg = config();
    cfg.consent_granted = false;
    cfg.synthetic_fixture = true;
    V380SourceAdapter adapter(cfg);
    std::string error;
    REQUIRE(adapter.open(&error) == V380Refusal::consent_withheld);
    REQUIRE(error.find("nothing is queued") != std::string::npos);
    REQUIRE(adapter.buffered_frames() == 0);
}

TEST_CASE("v380: the synthetic fixture path is bounded and never claims to be the camera",
          "[v380][fixture][bounds]") {
    auto cfg = config();
    cfg.synthetic_fixture = true;
    cfg.fixture_frames = 4;
    cfg.buffer.max_frames = 4;
    V380SourceAdapter adapter(cfg);
    std::string error;
    REQUIRE(adapter.open(&error) == V380Refusal::ok);

    size_t produced = 0;
    MediaFrame frame;
    for (int i = 0; i < 20; ++i) {
        if (adapter.read_frame(&frame, nullptr, nullptr)) ++produced;
    }
    // The fixture run is bounded, and the media path stayed bounded with it.
    REQUIRE(produced <= 4);
    REQUIRE(adapter.stats().frames_high_water <= 4);
    REQUIRE(adapter.stats().bytes_high_water <= cfg.buffer.max_bytes);
    // And no frame became anything else: the fixture is still a fixture, on a device capability that is
    // still unverified.
    REQUIRE_FALSE(adapter.capabilities().local_interface_available);
    REQUIRE_FALSE(adapter.capabilities().verified_by_this_worker);
}

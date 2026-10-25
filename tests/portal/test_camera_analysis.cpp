#include <catch2/catch.hpp>
#include "bridge/CameraAnalysis.h"
#include "core/AlertPolicy.h"
#include "edge/FrameConsumer.h"
#include <stdexcept>
#include <fstream>
#include <functional>

#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace homeguardian;

namespace {

// TEST-ONLY fixture generator. The production decoder never runs an encoder; this exists so the
// positive case is a real ffmpeg JPEG rather than a hand-built byte string.
std::vector<uint8_t> make_jpeg(const std::vector<uint8_t>& luma) {
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) return {};
    pid_t pid = fork();
    if (pid == 0) {
        // The child must not hold the parent's ends: keeping in[1] open here means the encoder
        // never sees EOF and the fixture deadlocks instead of failing.
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) { dup2(null_fd, 2); close(null_fd); }
        alarm(10);
        execl("/usr/bin/ffmpeg", "ffmpeg", "-nostdin", "-v", "error",
              "-f", "rawvideo", "-pix_fmt", "gray", "-s", "160x120", "-i", "pipe:0",
              "-frames:v", "1", "-f", "mjpeg", "pipe:1", (char*)nullptr);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    if (pid < 0) { close(in[1]); close(out[0]); return {}; }
    size_t sent = 0;
    while (sent < luma.size()) {
        ssize_t n = write(in[1], luma.data() + sent, luma.size() - sent);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    close(in[1]);
    std::vector<uint8_t> jpeg;
    uint8_t buf[4096];
    ssize_t n;
    while ((n = read(out[0], buf, sizeof(buf))) > 0) jpeg.insert(jpeg.end(), buf, buf + n);
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    return jpeg;
}

// A 40x40 bright rectangle at `left`; -1 draws the empty scene (the baseline).
std::vector<uint8_t> scene(int left) {
    std::vector<uint8_t> gray(160 * 120, 40);
    if (left >= 0)
        for (int y = 40; y < 80; ++y)
            for (int x = left; x < left + 40 && x < 160; ++x)
                gray[static_cast<size_t>(y) * 160 + x] = 230;
    return gray;
}

std::string write_registry(const std::string& path, const std::string& credential, bool capture) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return {};
    std::fprintf(f,
                 "{\"devices\":[{\"device_id\":\"camera-syn-001\",\"device_type\":\"simulator\","
                 "\"credential_id\":\"%s\",\"paired\":true,\"has_camera\":true,"
                 "\"consent_granted\":true,\"consent_purpose\":\"home_security\","
                 "\"capture_enabled\":%s}]}",
                 credential.c_str(), capture ? "true" : "false");
    std::fclose(f);
    return path;
}

// Models a ring/queued-work owner: the notification invalidates a generation,
// not just the current borrowed view. No media is encoded or published here.
struct FrameProbe : hg::edge::FrameConsumer {
    std::vector<uint8_t> retained;
    std::vector<bool> authority;
    hg::edge::BorrowedFrame last;
    uint64_t generation = 0;
    size_t calls = 0;
    std::function<void()> after_frame;
    bool throw_frame = false;
    bool throw_authority = false;
    void on_frame(const hg::edge::BorrowedFrame& f) override {
        ++calls;
        last = f;
        retained.assign(f.data, f.data + f.size);
        if (after_frame) after_frame();
        if (throw_frame) throw std::runtime_error("test consumer failure");
    }
    void on_authority_changed(bool allowed) override {
        authority.push_back(allowed);
        if (!allowed) { retained.clear(); ++generation; }
        if (throw_authority) throw std::runtime_error("test notification failure");
    }
};

// A header-valid but undecodable JPEG makes callback-before-decode observable.
std::vector<uint8_t> header_jpeg(int width = 640, int height = 360) {
    return {0xff,0xd8,0xff,0xc0,0,11,8,
            static_cast<uint8_t>(height >> 8),static_cast<uint8_t>(height),
            static_cast<uint8_t>(width >> 8),static_cast<uint8_t>(width),
            1,1,0x11,0,0xff,0xd9};
}

}  // namespace

TEST_CASE("Camera callback checks headers before decode and preserves acquisition metadata", "[camera-analysis][frame-callback]") {
    const std::string path = std::filesystem::temp_directory_path().string() + "/hg_camera_analysis_test/callback.json";
    const std::string credential(64, 'd');
    REQUIRE_FALSE(write_registry(path, credential, true).empty());
    auto probe = std::make_shared<FrameProbe>();
    portal::CameraAnalysis analysis(path, "camera-syn-001", credential, nullptr, probe);
    const auto captured = std::chrono::system_clock::now() - std::chrono::seconds(30);
    auto jpeg = header_jpeg();
    analysis.process(jpeg, captured, 19);
    REQUIRE(probe->calls == 1);
    REQUIRE(probe->last.captured_at == captured);
    REQUIRE(probe->last.sequence == 19);
    REQUIRE(probe->last.width == 640);
    REQUIRE(probe->last.height == 360);
    REQUIRE(probe->last.validated);
    REQUIRE(probe->last.data == jpeg.data());
    REQUIRE(probe->retained == jpeg);
    REQUIRE(analysis.status()["decode_failures"] == 1); // callback already ran
    const auto retained = probe->retained;
    std::fill(jpeg.begin(), jpeg.end(), 0);
    REQUIRE(probe->retained == retained); // call-scoped borrow, retained copy owns bytes

    for (const auto& invalid : std::vector<std::vector<uint8_t>>{
            {}, {0xff,0xd8,0xff,0xd9}, std::vector<uint8_t>(512u*1024u+1u, 0xff),
            header_jpeg(0,360), header_jpeg(4097,360), header_jpeg(640,4097),
            {0xff,0xd8,0xff,0xe0,0xff,0xff,0,0,0,0,0xff,0xd9}}) {
        analysis.process(invalid, captured, 20);
        REQUIRE(probe->calls == 1);
    }
    auto bad_magic = header_jpeg(); bad_magic[0] = 0;
    analysis.process(bad_magic, captured, 21);
    auto bad_end = header_jpeg(); bad_end.back() = 0;
    analysis.process(bad_end, captured, 22);
    REQUIRE(probe->calls == 1);

    // The legacy entry point may still analyze but must never invent ring time/sequence.
    analysis.process(header_jpeg());
    analysis.process(header_jpeg(), {}, 23);
    analysis.process(header_jpeg(), captured, 0);
    REQUIRE(probe->calls == 1);
    REQUIRE(probe->retained.empty());
    analysis.process(header_jpeg(4096,4096), captured, 24);
    REQUIRE(probe->calls == 2);
    auto exact_limit = header_jpeg();
    exact_limit.resize(512u*1024u, 0);
    exact_limit[exact_limit.size()-2] = 0xff; exact_limit.back() = 0xd9;
    analysis.process(exact_limit, captured, 25);
    REQUIRE(probe->calls == 3);
}

TEST_CASE("Camera callback delivers invalidation notifications on lifecycle changes", "[camera-analysis][frame-callback]") {
    const std::string path = std::filesystem::temp_directory_path().string() + "/hg_camera_analysis_test/callback_lifecycle.json";
    const std::string credential(64, 'e');
    REQUIRE_FALSE(write_registry(path, credential, true).empty());
    auto probe = std::make_shared<FrameProbe>();
    const auto captured = std::chrono::system_clock::now();
    {
        portal::CameraAnalysis analysis(path, "camera-syn-001", credential, nullptr, probe);
        analysis.process(header_jpeg(), captured, 1);
        REQUIRE_FALSE(probe->retained.empty());
        const auto job_generation = probe->generation;
        REQUIRE_FALSE(write_registry(path, credential, false).empty());
        REQUIRE_FALSE(analysis.authorize());
        REQUIRE(probe->retained.empty());
        REQUIRE(probe->generation > job_generation);
        analysis.process(header_jpeg(), captured, 2);
        REQUIRE(probe->calls == 1);

        REQUIRE_FALSE(write_registry(path, credential, true).empty());
        analysis.process(header_jpeg(), captured, 3);
        REQUIRE(probe->calls == 2);
        analysis.withdraw();
        REQUIRE(probe->retained.empty());
        analysis.process(header_jpeg(), captured, 4);
        REQUIRE_FALSE(probe->retained.empty());

        // Still authorized, but consent/enrollment context changed under the same id.
        nlohmann::json registry;
        { std::ifstream in(path); in >> registry; }
        registry["devices"][0]["consent_subject"] = "replacement-test-subject";
        registry["devices"][0]["consent_updated_ms"] = 1;
        { std::ofstream out(path); out << registry; }
        const auto old_generation = probe->generation;
        REQUIRE(analysis.authorize());
        REQUIRE(probe->generation > old_generation);
        REQUIRE(probe->retained.empty());
        analysis.process(header_jpeg(), captured, 5);
        REQUIRE_FALSE(probe->retained.empty());
    }
    REQUIRE(probe->retained.empty());
    REQUIRE_FALSE(probe->authority.back()); // shutdown/source replacement

    // Missing registry and credential rebind also withdraw, rather than keeping old bytes.
    {
        portal::CameraAnalysis analysis(path, "camera-syn-001", credential, nullptr, probe);
        analysis.process(header_jpeg(), captured, 6);
        REQUIRE_FALSE(write_registry(path, std::string(64, 'f'), true).empty());
        REQUIRE_FALSE(analysis.authorize());
        REQUIRE(probe->retained.empty());
        REQUIRE_FALSE(write_registry(path, credential, true).empty());
        analysis.process(header_jpeg(), captured, 7);
        REQUIRE(std::remove(path.c_str()) == 0);
        REQUIRE_FALSE(analysis.authorize());
        REQUIRE(probe->retained.empty());
    }
}

TEST_CASE("Camera callback rechecks authority even when JPEG decoding fails", "[camera-analysis][frame-callback]") {
    const std::string path = std::filesystem::temp_directory_path().string() + "/hg_camera_analysis_test/callback_decode_revocation.json";
    const std::string credential(64, 'e');
    REQUIRE_FALSE(write_registry(path, credential, true).empty());
    auto probe = std::make_shared<FrameProbe>();
    portal::CameraAnalysis analysis(path, "camera-syn-001", credential, nullptr, probe);
    bool withdrawn = false;
    SECTION("withdrawal during callback") {
        withdrawn = true;
        probe->after_frame = [&] { write_registry(path, credential, false); };
    }
    SECTION("authorized enrollment replacement during callback") {
        probe->after_frame = [&] {
            nlohmann::json registry;
            { std::ifstream in(path); in >> registry; }
            registry["devices"][0]["consent_updated_ms"] = 1;
            { std::ofstream out(path); out << registry; }
        };
    }
    const auto generation = probe->generation;
    analysis.process(header_jpeg(), std::chrono::system_clock::now(), 1);
    REQUIRE(probe->calls == 1);
    REQUIRE(analysis.status()["decode_failures"] == 1);
    REQUIRE(probe->generation > generation);
    REQUIRE(probe->retained.empty());
    REQUIRE(analysis.status()["authorized"] == !withdrawn);
    if (withdrawn) REQUIRE(analysis.status()["state"] == "authorization_denied");
}

TEST_CASE("Camera callback feeds the bounded EventRing without retaining the borrowed buffer", "[camera-analysis][frame-callback]") {
    const std::string path = std::filesystem::temp_directory_path().string() + "/hg_camera_analysis_test/callback_ring.json";
    const std::string credential(64, 'd');
    REQUIRE_FALSE(write_registry(path, credential, true).empty());
    auto jpeg = make_jpeg(scene(60));
    REQUIRE_FALSE(jpeg.empty());
    const auto original = jpeg;
    hg::edge::RingBounds bounds;
    bounds.max_frames = 2;
    bounds.max_total_bytes = jpeg.size()*2;
    hg::edge::EventRing ring(bounds);
    auto consumer = std::make_shared<hg::edge::RingFrameConsumer>(ring);
    const auto captured = std::chrono::system_clock::now() - std::chrono::seconds(30);
    {
        portal::CameraAnalysis analysis(path, "camera-syn-001", credential, nullptr, consumer);
        analysis.process(jpeg, captured, 1);
        REQUIRE(consumer->counts().accepted == 1);
        REQUIRE(analysis.status()["observations_processed"] == 1);
        // Supplying old acquisition time cannot rewind the unchanged detector clock.
        REQUIRE(analysis.status()["last_observation"]["timestamp_ms"].get<int64_t>() >
                std::chrono::duration_cast<std::chrono::milliseconds>(captured.time_since_epoch()).count());
        std::fill(jpeg.begin(), jpeg.end(), 0);
        auto window = ring.window(captured, captured);
        REQUIRE(window.ok);
        REQUIRE(window.frames.size() == 1);
        REQUIRE(window.frames[0].bytes == original);
        REQUIRE(window.frames[0].captured_at == captured);
        REQUIRE(window.frames[0].width == 160);
        REQUIRE(window.frames[0].height == 120);
        analysis.process(original, captured + std::chrono::milliseconds(100), 3);
        analysis.process(original, captured + std::chrono::milliseconds(200), 8);
        window = ring.window(captured, captured + std::chrono::milliseconds(200));
        REQUIRE(window.frames.size() == 2);
        REQUIRE(window.frames[0].sequence == 3);
        REQUIRE(window.frames[1].sequence == 8); // acquisition gaps are preserved
        REQUIRE(ring.stats().bytes <= bounds.max_total_bytes);
        analysis.withdraw();
        REQUIRE(ring.stats().frames == 0);
        REQUIRE_FALSE(ring.authorized());
        analysis.process(original, captured + std::chrono::milliseconds(300), 9);
        REQUIRE(ring.stats().frames == 1);
    }
    REQUIRE(ring.stats().frames == 0);
    REQUIRE_FALSE(ring.authorized());
}

TEST_CASE("Camera callback failure cannot bypass analysis or escape shutdown", "[camera-analysis][frame-callback]") {
    const std::string path = std::filesystem::temp_directory_path().string() + "/hg_camera_analysis_test/callback_failure.json";
    const std::string credential(64, 'f');
    REQUIRE_FALSE(write_registry(path, credential, true).empty());
    auto probe = std::make_shared<FrameProbe>();
    const auto captured = std::chrono::system_clock::now();
    {
        portal::CameraAnalysis analysis(path, "camera-syn-001", credential, nullptr, probe);
        probe->throw_frame = true;
        REQUIRE_NOTHROW(analysis.process(header_jpeg(), captured, 1));
        REQUIRE(probe->retained.empty());
        REQUIRE(analysis.status()["decode_failures"] == 1);
        REQUIRE_NOTHROW(analysis.process(header_jpeg(), captured, 2));
        REQUIRE(probe->calls == 1); // faulty callback stays disabled
        probe->throw_authority = true;
    }
    REQUIRE(probe->retained.empty());
}

TEST_CASE("Camera JPEG decode is bounded and yields the bounded luma plane", "[camera-analysis]") {
    const auto jpeg = make_jpeg(scene(60));
    REQUIRE_FALSE(jpeg.empty());
    const auto luma = portal::decode_jpeg_luma(jpeg);
    REQUIRE(luma.size() == 160u * 120u);
    REQUIRE(luma[40 * 160 + 80] > 180);   // the bright rectangle survived
    REQUIRE(luma[5 * 160 + 5] < 90);      // the dark field survived
    REQUIRE(portal::decode_jpeg_luma({}).empty());
    REQUIRE(portal::decode_jpeg_luma(std::vector<uint8_t>(600 * 1024, 0xFF)).empty());
}

TEST_CASE("Camera acquisition is rate limited on the server, not only in the browser", "[camera-analysis]") {
    portal::CameraConfig cfg;   // no host: nothing can be fetched, so only the cadence gate runs
    portal::LiveCameraFeed feed(cfg);
    REQUIRE(feed.fetch_frame(1000).error == "plaintext_not_enabled");
    REQUIRE(feed.fetch_frame(1100).error == "rate_limited");
    REQUIRE(feed.fetch_frame(2000).error == "plaintext_not_enabled");   // one attempt per interval
}

TEST_CASE("Camera analysis fails closed without an authorized source", "[camera-analysis]") {
    const std::string base = std::filesystem::temp_directory_path().string() + "/hg_camera_analysis_test";
    const std::string registry = base + "/registry_closed.json";
    const std::string credential(64, 'b');
    // Capture still disabled by the operator: consent and pairing are not permission to collect.
    REQUIRE_FALSE(write_registry(registry, credential, false).empty());
    portal::CameraAnalysis analysis(registry, "camera-syn-001", credential, nullptr);
    REQUIRE_FALSE(analysis.authorize());
    REQUIRE(analysis.status()["authorized"] == false);
    REQUIRE(analysis.status()["state"] == "authorization_denied");

    // Missing registry file: fail closed, never "no authorization required".
    portal::CameraAnalysis missing(base + "/absent.json", "camera-syn-001",
                                   credential, nullptr);
    REQUIRE_FALSE(missing.authorize());

    // Wrong credential: pairing is not identity.
    portal::CameraAnalysis other(registry, "camera-syn-001", std::string(64, 'c'), nullptr);
    REQUIRE_FALSE(other.authorize());
}

TEST_CASE("Camera analysis commits a qualified alert to the durable store", "[camera-analysis]") {
    const std::string dir = std::filesystem::temp_directory_path().string() + "/hg_camera_analysis_test";
    const std::string registry = dir + "/registry.json";
    const std::string credential(64, 'a');
    REQUIRE_FALSE(write_registry(registry, credential, true).empty());
    PersistenceAudit audit(dir + "/audit.db");
    portal::CameraAnalysis analysis(registry, "camera-syn-001", credential, &audit);

    // Chronologically coherent AND in the past: the pipeline rejects evidence more than 1s in
    // the future, and a fixture whose frames never reach the engine proves nothing.
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 40 * 600 - 3000;
    bool alerted = false;
    for (int i = 0; i < 40; ++i) {
        // Chronology that actually reaches the rule: 12 empty frames establish the baseline (the
        // parent fixture's opening), 18 frames hold a static rectangle (the scene settles, so the
        // baseline adapts), then the rectangle MOVES every frame. The movement frames are fed at
        // the real acquisition cadence, so they are genuinely inside the rule's 5s correlation
        // window; a fixture whose frames sit outside that window proves nothing about the rule.
        const int left = i < 12 ? -1 : (i < 30 ? 60 : 10 + (i - 30) * 13);
        if (i >= 30) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        } else {
            now += 600;
        }
        const auto jpeg = make_jpeg(scene(left));
        REQUIRE_FALSE(jpeg.empty());
        analysis.process(jpeg);
        const auto status = analysis.status();
        REQUIRE(status["provenance"] == "synthetic");
        REQUIRE(status["constraints"]["no_identity_inference"] == true);
        if (status["alerts_committed"].get<int>() > 0) alerted = true;
    }
    const auto status = analysis.status();
    INFO(status.dump());
    REQUIRE(status["observations_processed"] == 40);
    REQUIRE(status["commit_failures"] == 0);
    REQUIRE(alerted);
    REQUIRE(status["alerts_committed"].get<int>() >= 1);
    REQUIRE(audit.total_alert_count() >= 1);
    const std::string alert_id = status["last_alert_id"].get<std::string>();
    REQUIRE_FALSE(alert_id.empty());
    REQUIRE(audit.alerts().find_recent(10).front().alert_id == alert_id);

    // Revocation takes effect on the next acquisition and resets the baseline.
    REQUIRE_FALSE(write_registry(registry, credential, false).empty());
    analysis.process(make_jpeg(scene(10)));
    REQUIRE(analysis.status()["authorized"] == false);
    REQUIRE(analysis.status()["state"] == "authorization_denied");
}

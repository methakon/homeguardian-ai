#include <catch2/catch.hpp>
#include "bridge/CameraAnalysis.h"
#include "core/AlertPolicy.h"

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

}  // namespace

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

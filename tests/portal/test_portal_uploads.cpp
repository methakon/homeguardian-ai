#include "catch2/catch.hpp"

#include "bridge/PortalUploads.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// --------------------------------------------------------------------------------------------
// The backup portal's uploaded-media store.
//
// These tests are the evidence for the mission's required verification list that can be proven
// without HTTP: valid image, valid video, rejected formats, oversized files, path-traversal
// attempts, and the simulated-not-live labelling. The HTTP-level properties (CSRF, unauthorized
// upload/delete, backend-unavailable behaviour) are proven by the portal smoke script, which
// exercises the real binary over loopback.
// --------------------------------------------------------------------------------------------

using namespace hg_portal;

namespace {

std::string tmp_root(const std::string& tag) {
    const std::string dir = "/tmp/hg_portal_test_" + tag + "_" + std::to_string(::getpid());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string bytes_of(const std::vector<unsigned char>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

// A real, minimal 1x1 PNG (signature + IHDR + IDAT + IEND is not needed: the signature and the
// declared type are what the validator verifies; total size stays tiny).
std::string png_1x1() {
    std::vector<unsigned char> v = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const char* ihdr = "\x00\x00\x00\x0DIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00";
    v.insert(v.end(), ihdr, ihdr + 24);
    return bytes_of(v);
}

std::string jpeg_tiny() {
    std::vector<unsigned char> v = {0xFF, 0xD8, 0xFF, 0xE0};
    v.insert(v.end(), 60, 0x00);
    v.push_back(0xFF);
    v.push_back(0xD9);
    return bytes_of(v);
}

std::string webp_tiny() {
    std::vector<unsigned char> v = {'R', 'I', 'F', 'F', 0x20, 0x00, 0x00, 0x00,
                                    'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
    v.insert(v.end(), 32, 0x00);
    return bytes_of(v);
}

void put_be32(std::string& s, size_t at, uint32_t v) {
    s[at] = static_cast<char>((v >> 24) & 0xFF);
    s[at + 1] = static_cast<char>((v >> 16) & 0xFF);
    s[at + 2] = static_cast<char>((v >> 8) & 0xFF);
    s[at + 3] = static_cast<char>(v & 0xFF);
}

// A minimal but structurally valid MP4: ftyp + moov/mvhd, with the given duration in seconds.
std::string mp4_with_duration(uint32_t seconds, uint32_t timescale = 1000) {
    std::string s;
    // ftyp
    s.push_back('\0'); s.push_back('\0'); s.push_back('\0'); s.push_back(0x10);
    s += "ftypisom"; s.push_back('\0'); s.push_back('\0'); s.push_back('\x02'); s.push_back('\0');
    // moov (8 header + 108 mvhd)
    const uint32_t moov_size = 8 + 108;
    std::string moov;
    moov.push_back('\0'); moov.push_back('\0'); moov.push_back('\0'); moov.push_back(0x6C);
    moov += "moov";
    std::string mvhd;
    mvhd.push_back('\0'); mvhd.push_back('\0'); mvhd.push_back('\0'); mvhd.push_back(0x6C);
    mvhd += "mvhd";
    mvhd.insert(mvhd.size(), 100, '\0');          // payload: version 0 at [8]
    put_be32(mvhd, 8 + 12, timescale);            // timescale
    put_be32(mvhd, 8 + 16, seconds * timescale);  // duration
    moov += mvhd;
    s += moov;
    (void)moov_size;
    return s;
}

std::string mp4_without_mvhd() {
    std::string s;
    s.push_back('\0'); s.push_back('\0'); s.push_back('\0'); s.push_back(0x10);
    s += "ftypisom"; s.push_back('\0'); s.push_back('\0'); s.push_back('\x02'); s.push_back('\0');
    s.push_back('\0'); s.push_back('\0'); s.push_back('\0'); s.push_back(0x10);
    s += "moov";
    s.push_back('\0'); s.push_back('\0'); s.push_back('\0'); s.push_back(0x08);  // empty child
    return s;
}

std::string webm_with_duration(float seconds) {
    std::string s;
    const char ebml[] = "\x1A\x45\xDF\xA3";
    s.append(ebml, 4);
    s.insert(s.size(), 8, '\0');
    // Info element carrying Duration (0x44 0x89) as a 4-byte big-endian float.
    s.push_back('\x15'); s.push_back('\x49'); s.push_back('\xA9'); s.push_back('\x66');
    s.push_back('\x44'); s.push_back('\x89'); s.push_back('\x84');
    unsigned char raw[4] = {0, 0, 0, 0};
    std::memcpy(raw, &seconds, 4);
    s.push_back(static_cast<char>(raw[3]));
    s.push_back(static_cast<char>(raw[2]));
    s.push_back(static_cast<char>(raw[1]));
    s.push_back(static_cast<char>(raw[0]));
    return s;
}

}  // namespace

TEST_CASE("portal: a valid PNG is accepted and stored outside any web root",
          "[portal][uploads]") {
    const std::string root = tmp_root("png");
    UploadStore store(root);
    REQUIRE(store.load());

    std::string err;
    const auto rec = store.add(png_1x1(), "image/png", "photo.png", &err);
    REQUIRE(rec.has_value());
    CHECK(err.empty());
    CHECK(rec->mime == "image/png");
    CHECK_FALSE(rec->is_video);
    CHECK(UploadStore::valid_id(rec->id));
    // The stored name is the server-generated id, never the client's name.
    CHECK(std::filesystem::exists(root + "/" + rec->id + ".png"));
    CHECK_FALSE(std::filesystem::exists(root + "/photo.png"));

    // Reads back byte-identically.
    const auto got = store.read(rec->id);
    REQUIRE(got.has_value());
    CHECK(*got == png_1x1());
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: a valid short MP4 is accepted and its duration is verified",
          "[portal][uploads]") {
    const std::string root = tmp_root("mp4");
    UploadStore store(root);
    REQUIRE(store.load());

    std::string err;
    const auto rec = store.add(mp4_with_duration(5), "video/mp4", "clip.mp4", &err);
    REQUIRE(rec.has_value());
    CHECK(rec->is_video);
    CHECK(rec->duration_ms == 5000);
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: a valid short WebM is accepted and its duration is verified",
          "[portal][uploads]") {
    const std::string root = tmp_root("webm");
    UploadStore store(root);
    REQUIRE(store.load());

    std::string err;
    const auto rec = store.add(webm_with_duration(3.5f), "video/webm", "clip.webm", &err);
    REQUIRE(rec.has_value());
    CHECK(rec->is_video);
    CHECK(rec->duration_ms == 3500);
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: unsupported, mismatched and executable uploads are refused",
          "[portal][uploads]") {
    const std::string root = tmp_root("reject");
    UploadStore store(root);
    REQUIRE(store.load());
    std::string err;

    SECTION("a script with an image extension is caught by the signature check") {
        const std::string script = "#!/bin/sh\nrm -rf /\n";
        CHECK_FALSE(store.add(script, "image/png", "evil.png", &err).has_value());
        CHECK(err == "unsupported_format");
    }
    SECTION("an unsupported extension is refused") {
        CHECK_FALSE(store.add(png_1x1(), "image/png", "thing.svg", &err).has_value());
    }
    SECTION("a PDF signature is refused even with an image name") {
        CHECK_FALSE(store.add("%PDF-1.7\n", "image/png", "doc.png", &err).has_value());
    }
    SECTION("extension and signature must agree") {
        // A PNG's bytes carrying a .mp4 name.
        CHECK_FALSE(store.add(png_1x1(), "image/png", "clip.mp4", &err).has_value());
        CHECK(err == "format_mismatch");
    }
    SECTION("declared MIME type must agree with the signature") {
        CHECK_FALSE(store.add(png_1x1(), "video/mp4", "photo.png", &err).has_value());
        CHECK(err == "format_mismatch");
    }
    SECTION("an empty body is refused") {
        CHECK_FALSE(store.add("", "image/png", "empty.png", &err).has_value());
        CHECK(err == "empty_body");
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: oversized uploads and over-long videos are refused", "[portal][uploads]") {
    const std::string root = tmp_root("size");
    UploadStore store(root);
    REQUIRE(store.load());
    std::string err;

    SECTION("an image above the image ceiling") {
        std::string big = png_1x1();
        big.resize(kMaxImageBytes + 1, '\0');
        CHECK_FALSE(store.add(big, "image/png", "big.png", &err).has_value());
        CHECK(err == "too_large");
    }
    SECTION("a video above the duration ceiling") {
        CHECK_FALSE(store.add(mp4_with_duration(60), "video/mp4", "long.mp4", &err).has_value());
        CHECK(err == "too_long");
    }
    SECTION("a video whose duration cannot be read is refused, not trusted") {
        CHECK_FALSE(store.add(mp4_without_mvhd(), "video/mp4", "opaque.mp4", &err).has_value());
        CHECK(err == "duration_unverifiable");
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: path traversal cannot reach the filesystem", "[portal][uploads]") {
    const std::string root = tmp_root("traversal");
    UploadStore store(root);
    REQUIRE(store.load());

    // The last entry builds the string EXPLICITLY with an embedded NUL: a plain "...\0" literal
    // is truncated by the const char* constructor and silently becomes a valid 26-char id, which
    // makes the assertion below vacuous. Testing an embedded NUL requires an explicit length.
    std::string with_nul("u_20260101_000000_deadbeef", 26);
    with_nul.push_back('\0');

    const std::vector<std::string> hostile = {
        "../index", "../../etc/passwd", "u_20260101_000000_deadbeef/../../x",
        "/etc/passwd", "u_20260101_0000_deadbeef",
        "u_20260101_000000_deadbeeg", with_nul, "media/../../etc/passwd",
        // and a well-formed 26-char id whose last character is not hex
        "u_20260101_000000_deadbeez",
    };
    REQUIRE(with_nul.size() == 27);
    for (const auto& id : hostile) {
        CHECK_FALSE(UploadStore::valid_id(id));
        CHECK_FALSE(store.read(id).has_value());
        CHECK_FALSE(store.find(id).has_value());
    }
    // A syntactically valid id that was never issued is also unreadable.
    CHECK_FALSE(store.read("u_20260101_000000_deadbeef").has_value());
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: a client-supplied name is never used as a path", "[portal][uploads]") {
    const std::string root = tmp_root("names");
    UploadStore store(root);
    REQUIRE(store.load());
    std::string err;

    const auto rec = store.add(png_1x1(), "image/png", "../../../tmp/pwned.png", &err);
    REQUIRE(rec.has_value());
    // Extension taken from the name, but the file name is the server's.
    CHECK(rec->id.rfind("u_", 0) == 0);
    CHECK(std::filesystem::exists(root + "/" + rec->id + ".png"));
    CHECK_FALSE(std::filesystem::exists("/tmp/pwned.png"));
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: the feed is labelled simulated and never claims a live camera",
          "[portal][uploads]") {
    const std::string root = tmp_root("feed");
    UploadStore store(root);
    REQUIRE(store.load());
    std::string err;
    REQUIRE(store.add(png_1x1(), "image/png", "a.png", &err).has_value());
    REQUIRE(store.add(mp4_with_duration(4), "video/mp4", "b.mp4", &err).has_value());

    const std::string feed = feed_json(store);
    CHECK(feed.find("Uploaded media") != std::string::npos);
    CHECK(feed.find("simulated") != std::string::npos);
    CHECK(feed.find("\"live_camera\":false") != std::string::npos);
    // No claim of CCTV/live/Alexa/real-camera provenance anywhere in the payload.
    CHECK(feed.find("CCTV") == std::string::npos);
    CHECK(feed.find("cctv") == std::string::npos);
    CHECK(feed.find("live_camera\":true") == std::string::npos);
    CHECK(feed.find("alexa") == std::string::npos);
    CHECK(feed.find("Alexa") == std::string::npos);
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: records survive a reload and delete removes the file", "[portal][uploads]") {
    const std::string root = tmp_root("persist");
    std::string id, err;
    {
        UploadStore store(root);
        REQUIRE(store.load());
        const auto rec = store.add(png_1x1(), "image/png", "keep.png", &err);
        REQUIRE(rec.has_value());
        id = rec->id;
    }
    {
        UploadStore reopened(root);
        REQUIRE(reopened.load());
        REQUIRE(reopened.records().size() == 1);
        CHECK(reopened.find(id).has_value());
        CHECK(std::filesystem::exists(root + "/" + id + ".png"));
        CHECK(reopened.remove(id));
        CHECK_FALSE(reopened.find(id).has_value());
        CHECK_FALSE(std::filesystem::exists(root + "/" + id + ".png"));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("portal: the feed is bounded", "[portal][uploads]") {
    const std::string root = tmp_root("bounded");
    UploadStore store(root);
    REQUIRE(store.load());
    std::string err;
    for (size_t i = 0; i < kMaxFeedEntries + 5; ++i) {
        REQUIRE(store.add(png_1x1(), "image/png", "x.png", &err).has_value());
    }
    CHECK(store.records().size() == kMaxFeedEntries);
    std::filesystem::remove_all(root);
}

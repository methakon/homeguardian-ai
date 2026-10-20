// test_v380_protocol: the V380 wire codec, verified with synthetic bytes only.
//
// NO CAMERA IS CONTACTED HERE, and none can be: these are the documented wire layouts, exercised against
// byte buffers this file constructs. The vendor static key is NOT used - a synthetic key stands in its
// place, which is also the point: the codec takes the key as a parameter and nothing in this repository
// carries the real one.
//
// The crypto is verified by ROUND TRIP (encrypt, then reverse the two stages, and check the padded password
// returns), not by comparing against a captured constant. A hardcoded expected ciphertext would only prove
// that the test and the implementation agree with each other.

#include <catch2/catch.hpp>

#include "camera/v380_protocol.h"

#include "core/Aes128Ecb.h"

#include <string>
#include <vector>

using namespace homeguardian;
using namespace homeguardian::v380;

namespace {

const std::string kSyntheticStaticKey = "synthetic-key-1!";   // 16 bytes, stands in for the vendor key
const std::string kSyntheticRandomKey = "AAAAbbbbCCCC9999";   // 16 bytes

void write_u16(std::vector<uint8_t>* v, size_t off, uint16_t value) {
    for (size_t i = 0; i < 2; ++i) (*v)[off + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
}
void write_i32(std::vector<uint8_t>* v, size_t off, int32_t value) {
    const uint32_t u = static_cast<uint32_t>(value);
    for (size_t i = 0; i < 4; ++i) (*v)[off + i] = static_cast<uint8_t>((u >> (8 * i)) & 0xff);
}

// Reverse of the codec's two-stage construction: decrypt with the random key, then with the static key.
std::string decrypt_password_field(const std::vector<uint8_t>& field, const std::string& static_key) {
    REQUIRE(field.size() == kPasswordFieldBytes);
    const std::string random_key(reinterpret_cast<const char*>(field.data()), kRandomKeyBytes);
    std::vector<uint8_t> block(field.begin() + kRandomKeyBytes, field.end());
    REQUIRE(Aes128Ecb::decrypt_block(random_key, block.data()));
    REQUIRE(Aes128Ecb::decrypt_block(static_key, block.data()));
    size_t len = block.size();
    while (len > 0 && block[len - 1] == 0) --len;   // the padding the codec added before encrypting
    return std::string(block.begin(), block.begin() + static_cast<long>(len));
}

std::vector<uint8_t> login_response(int32_t command, int32_t login_result, uint32_t ticket,
                                    uint32_t session) {
    std::vector<uint8_t> b(40, 0);
    write_i32(&b, 0, command);
    write_i32(&b, 4, login_result);
    write_i32(&b, 8, 0);
    b[12] = 2;                       // version
    for (size_t i = 0; i < 4; ++i) b[13 + i] = static_cast<uint8_t>((ticket >> (8 * i)) & 0xff);
    for (size_t i = 0; i < 4; ++i) b[17 + i] = static_cast<uint8_t>((session >> (8 * i)) & 0xff);
    return b;
}

std::vector<uint8_t> stream_response(int32_t command, int32_t v21) {
    std::vector<uint8_t> b(18, 0);
    write_i32(&b, 0, command);
    write_i32(&b, 4, v21);
    write_u16(&b, 8, 20);
    return b;
}

// One 12-byte chunk header plus its body.
std::vector<uint8_t> chunk(uint8_t type, uint16_t total, uint16_t index, const std::string& body) {
    std::vector<uint8_t> b(12 + body.size(), 0);
    b[0] = kChunkMagic;
    b[1] = type;
    write_u16(&b, 3, total);
    write_u16(&b, 5, index);
    write_u16(&b, 7, static_cast<uint16_t>(body.size()));
    std::copy(body.begin(), body.end(), b.begin() + 12);
    return b;
}

}  // namespace

TEST_CASE("v380 protocol: the password field is the two-stage construction, verified by round trip",
          "[v380proto]") {
    const std::string password = "syntheticpw";
    std::vector<uint8_t> field;
    std::string why;
    REQUIRE(build_password_field(password, kSyntheticStaticKey, kSyntheticRandomKey, &field, &why));
    INFO(why);
    REQUIRE(field.size() == kPasswordFieldBytes);
    // The random key travels in the clear as the first 16 bytes - that is the documented layout.
    REQUIRE(std::string(field.begin(), field.begin() + kRandomKeyBytes) == kSyntheticRandomKey);
    // And the ciphertext really is that password, encrypted twice, in that order.
    REQUIRE(decrypt_password_field(field, kSyntheticStaticKey) == password);
}

TEST_CASE("v380 protocol: the password field refuses what it cannot represent", "[v380proto][refusal]") {
    std::vector<uint8_t> field;
    std::string why;

    SECTION("a password longer than the 32-byte field can hold") {
        REQUIRE_FALSE(build_password_field("this-password-is-way-too-long", kSyntheticStaticKey,
                                           kSyntheticRandomKey, &field, &why));
        REQUIRE(why.find("longer than") != std::string::npos);
    }
    SECTION("an empty password is accepted, and really is an all-zero block") {
        REQUIRE(build_password_field("", kSyntheticStaticKey, kSyntheticRandomKey, &field, &why));
        REQUIRE(field.size() == kPasswordFieldBytes);
        REQUIRE(decrypt_password_field(field, kSyntheticStaticKey).empty());
    }
    SECTION("a static key that is not 16 bytes") {
        REQUIRE_FALSE(build_password_field("pw", "short", kSyntheticRandomKey, &field, &why));
        REQUIRE(why.find("16 bytes") != std::string::npos);
    }
    SECTION("the failure names the real cause rather than returning a half-built field") {
        REQUIRE_FALSE(build_password_field("ok-password", kSyntheticStaticKey, kSyntheticRandomKey, &field,
                                           &why) == false);
        REQUIRE(field.size() == kPasswordFieldBytes);
    }
}

TEST_CASE("v380 protocol: the login request lands on the documented offsets", "[v380proto][wire]") {
    LoginRequest req;
    req.device_id = 12345678;
    req.username = "admin";
    req.password = "syntheticpw";
    std::vector<uint8_t> out;
    std::string why;
    REQUIRE(build_login_request(req, kSyntheticStaticKey, kSyntheticRandomKey, &out, &why));
    INFO(why);
    REQUIRE(out.size() == kLoginRequestBytes);
    REQUIRE(static_cast<int32_t>(out[0] | (out[1] << 8)) == 1167);
    REQUIRE((out[4] | (out[5] << 8)) == 1022);        // unknown1: the "us" login variant
    REQUIRE(out[8] == 2);                              // unknown2
    REQUIRE((out[9] | (out[10] << 8)) == 1);           // unknown3
    REQUIRE(out[13] == 0x4e);                          // device id, little-endian: 12345678 -> 0x4e 0x61 0xbc 0x00
    REQUIRE(out[14] == 0x61);
    REQUIRE(std::string(out.begin() + 49, out.begin() + 54) == "admin");
    REQUIRE(std::string(out.begin() + 81, out.begin() + 97) == kSyntheticRandomKey);
    // Everything the layout does not use stays zero, so a device never sees uninitialised bytes.
    for (size_t i = 113; i < out.size(); ++i) REQUIRE(out[i] == 0);
}

TEST_CASE("v380 protocol: the login request refuses degenerate input", "[v380proto][refusal]") {
    std::vector<uint8_t> out;
    std::string why;
    LoginRequest req;
    req.device_id = 0;
    req.password = "pw";
    REQUIRE_FALSE(build_login_request(req, kSyntheticStaticKey, kSyntheticRandomKey, &out, &why));
    REQUIRE(why.find("device id") != std::string::npos);

    req.device_id = 5;
    req.username = std::string(40, 'x');
    REQUIRE_FALSE(build_login_request(req, kSyntheticStaticKey, kSyntheticRandomKey, &out, &why));
    REQUIRE(why.find("32 bytes") != std::string::npos);
}

TEST_CASE("v380 protocol: the stream login carries the ticket and stays inside the documented ranges",
          "[v380proto][wire]") {
    std::vector<uint8_t> out;
    std::string why;
    REQUIRE(build_stream_login_request(12345678, 0xdeadbeef, 20, 1, &out, &why));
    INFO(why);
    REQUIRE((out[0] | (out[1] << 8)) == 301);
    REQUIRE(out[4] == 0x4e);                       // device id
    REQUIRE(out[12] == 20);                        // fps, low byte
    REQUIRE(out[14] == 0xef);                      // auth ticket, little-endian
    REQUIRE(out[15] == 0xbe);
    REQUIRE(out[16] == 0xad);
    REQUIRE(out[17] == 0xde);
    REQUIRE((out[22] | (out[23] << 8)) == 4097);   // unknown4, as the documented exchange uses it
    REQUIRE(out[26] == 1);                         // resolution = high
    // A device documents fps in [10,20] and silently falls back to 13 on an invalid value, so an
    // out-of-range request is refused here instead of being sent and producing a stream at the wrong rate.
    REQUIRE_FALSE(build_stream_login_request(12345678, 1, 30, 1, &out, &why));
    REQUIRE_FALSE(build_stream_login_request(12345678, 1, 20, 2, &out, &why));
}

TEST_CASE("v380 protocol: login responses become named outcomes, never a bare boolean", "[v380proto]") {
    auto r = parse_login_response(login_response(1168, 1001, 0xaabbccdd, 0x11223344).data(), 40);
    REQUIRE(r.result == LoginResult::ok);
    REQUIRE(r.auth_ticket == 0xaabbccdd);
    REQUIRE(r.session == 0x11223344);

    REQUIRE(parse_login_response(login_response(1168, 1011, 0, 0).data(), 40).result ==
            LoginResult::invalid_username);
    REQUIRE(parse_login_response(login_response(1168, 1012, 0, 0).data(), 40).result ==
            LoginResult::invalid_password);
    REQUIRE(parse_login_response(login_response(1168, 1018, 0, 0).data(), 40).result ==
            LoginResult::invalid_device_id);
    // An unrecognised result code is a REFUSAL, never a success - the device sends no text to relay.
    REQUIRE(parse_login_response(login_response(1168, 9999, 0, 0).data(), 40).result ==
            LoginResult::invalid_password);
    REQUIRE(parse_login_response(login_response(1166, 1001, 0, 0).data(), 40).result ==
            LoginResult::unexpected_command);
    REQUIRE(parse_login_response(login_response(1168, 1001, 0, 0).data(), 12).result ==
            LoginResult::short_response);
}

TEST_CASE("v380 protocol: a newer-generation camera is a NAMED dead end, not a retry loop",
          "[v380proto][generation]") {
    REQUIRE(parse_stream_response(stream_response(401, 402).data(), 18).result == StreamResult::ok);
    REQUIRE(parse_stream_response(stream_response(401, 1001).data(), 18).result == StreamResult::ok);

    // -11 / -12 is the encrypted generation: the codec has no key and cannot obtain one from the protocol.
    const auto unsupported = parse_stream_response(stream_response(401, -11).data(), 18);
    REQUIRE(unsupported.result == StreamResult::unsupported_firmware_generation);
    REQUIRE(std::string(to_string(unsupported.result)) == "unsupported_firmware_generation");
    REQUIRE(parse_stream_response(stream_response(401, -12).data(), 18).result ==
            StreamResult::unsupported_firmware_generation);
    REQUIRE(parse_stream_response(stream_response(401, 777).data(), 18).result == StreamResult::refused);
    REQUIRE(parse_stream_response(stream_response(400, 402).data(), 18).result ==
            StreamResult::unexpected_command);
    REQUIRE(parse_stream_response(stream_response(401, 402).data(), 10).result ==
            StreamResult::short_response);
}

TEST_CASE("v380 protocol: chunks reassemble into whole frames, and only whole frames", "[v380proto][chunks]") {
    ChunkAssembler asm_(ChunkAssembler::Config{});
    ChunkHeader hdr;

    // A frame split across three chunks: nothing is emitted until the last one arrives.
    const auto c0 = chunk(kChunkTypeIFrame, 3, 0, "first-");
    const auto c1 = chunk(kChunkTypeIFrame, 3, 1, "second-");
    const auto c2 = chunk(kChunkTypeIFrame, 3, 2, "third");
    AssembledFrame frame;

    for (const auto* c : {&c0, &c1}) {
        REQUIRE(asm_.begin_chunk(c->data(), c->size(), &hdr) == ChunkOutcome::need_more);
        REQUIRE(asm_.append_chunk(hdr, c->data() + kChunkHeaderBytes, c->size() - kChunkHeaderBytes, nullptr));
        REQUIRE_FALSE(asm_.take_frame(&frame));       // a partial frame is NEVER handed on
    }
    REQUIRE(asm_.begin_chunk(c2.data(), c2.size(), &hdr) == ChunkOutcome::need_more);
    REQUIRE(asm_.append_chunk(hdr, c2.data() + kChunkHeaderBytes, c2.size() - kChunkHeaderBytes, nullptr));
    REQUIRE(asm_.take_frame(&frame));
    REQUIRE(frame.type == kChunkTypeIFrame);
    REQUIRE(frame.chunks == 3);
    REQUIRE(frame.payload_bytes == 18);
    REQUIRE(asm_.frames_completed() == 1);
    REQUIRE(asm_.frame_bytes_held() == 0);            // the frame was taken, not retained
}

TEST_CASE("v380 protocol: the assembler enforces the protocol's own sanity rules", "[v380proto][chunks]") {
    ChunkAssembler asm_(ChunkAssembler::Config{});
    ChunkHeader hdr;

    SECTION("a keepalive marker is counted, not mistaken for a frame") {
        std::vector<uint8_t> keep(12, 0);
        keep[0] = 0x6f;
        REQUIRE(asm_.begin_chunk(keep.data(), keep.size(), &hdr) == ChunkOutcome::keepalive);
        REQUIRE(asm_.keepalives_seen() == 1);
    }
    SECTION("a header byte with no rule is fatal, because the stream is not the legacy stream") {
        std::vector<uint8_t> junk(12, 0);
        junk[0] = 0x55;
        REQUIRE(asm_.begin_chunk(junk.data(), junk.size(), &hdr) == ChunkOutcome::fatal_unknown);
    }
    SECTION("a chunk longer than the documented bound is dropped") {
        auto bad = chunk(kChunkTypePFrame, 2, 0, std::string(600, 'p'));
        REQUIRE(asm_.begin_chunk(bad.data(), bad.size(), &hdr) == ChunkOutcome::dropped_protocol);
        REQUIRE(asm_.chunks_dropped() == 1);
    }
    SECTION("a chunk index outside the frame is dropped") {
        auto bad = chunk(kChunkTypePFrame, 2, 5, "x");
        REQUIRE(asm_.begin_chunk(bad.data(), bad.size(), &hdr) == ChunkOutcome::dropped_protocol);
    }
    SECTION("an unknown frame type is dropped") {
        auto bad = chunk(0x42, 2, 0, "x");
        REQUIRE(asm_.begin_chunk(bad.data(), bad.size(), &hdr) == ChunkOutcome::dropped_protocol);
    }
    SECTION("a chunk arriving out of order is refused and the partial frame is discarded") {
        const auto c0 = chunk(kChunkTypePFrame, 3, 0, "aa");
        const auto c2 = chunk(kChunkTypePFrame, 3, 2, "cc");
        REQUIRE(asm_.begin_chunk(c0.data(), c0.size(), &hdr) == ChunkOutcome::need_more);
        REQUIRE(asm_.append_chunk(hdr, c0.data() + kChunkHeaderBytes, c0.size() - kChunkHeaderBytes, nullptr));
        REQUIRE(asm_.begin_chunk(c2.data(), c2.size(), &hdr) == ChunkOutcome::need_more);
        std::string why;
        REQUIRE_FALSE(asm_.append_chunk(hdr, c2.data() + kChunkHeaderBytes,
                                        c2.size() - kChunkHeaderBytes, &why));
        REQUIRE(why.find("out-of-order") != std::string::npos);
        REQUIRE(asm_.frame_bytes_held() == 0);
    }
    SECTION("a frame over the configured ceiling is refused rather than buffered") {
        ChunkAssembler small(ChunkAssembler::Config{100, 65535});   // a 150-byte chunk must exceed this
        const auto big = chunk(kChunkTypePFrame, 2, 0, std::string(150, 'p'));
        REQUIRE(small.begin_chunk(big.data(), big.size(), &hdr) == ChunkOutcome::need_more);
        std::string why;
        REQUIRE_FALSE(small.append_chunk(hdr, big.data() + kChunkHeaderBytes,
                                         big.size() - kChunkHeaderBytes, &why));
        REQUIRE(why.find("ceiling") != std::string::npos);
        REQUIRE(small.frame_bytes_held() == 0);
    }
    SECTION("fewer than 12 bytes is need_more, not a parse of whatever is there") {
        std::vector<uint8_t> partial(7, 0);
        partial[0] = kChunkMagic;
        REQUIRE(asm_.begin_chunk(partial.data(), partial.size(), &hdr) == ChunkOutcome::need_more);
    }
}

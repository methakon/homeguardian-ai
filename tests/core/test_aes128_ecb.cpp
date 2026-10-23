// test_aes128_ecb: the in-repo cipher against the standard's own vectors.
//
// The point of this file is that it does NOT check the implementation against itself. The expected values
// below are the published FIPS-197 (AES) known-answer vectors, so a bug in key expansion, ShiftRows,
// MixColumns or the byte order fails here rather than silently producing a plausible-looking handshake that
// the camera rejects for reasons no log would explain.

#include <catch2/catch.hpp>

#include "core/Aes128Ecb.h"

#include <string>
#include <vector>

using namespace homeguardian;

namespace {

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string to_hex(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[(data[i] >> 4) & 0xf]);
        out.push_back(digits[data[i] & 0xf]);
    }
    return out;
}

std::string key_from_hex(const std::string& hex) {
    const auto bytes = from_hex(hex);
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace

TEST_CASE("aes128: FIPS-197 C.1 vector, both directions", "[aes][kat]") {
    // AES-128, key 000102...0f, plaintext 00112233445566778899aabbccddeeff
    const std::string key = key_from_hex("000102030405060708090a0b0c0d0e0f");
    auto block = from_hex("00112233445566778899aabbccddeeff");
    REQUIRE(Aes128Ecb::encrypt_block(key, block.data()));
    REQUIRE(to_hex(block.data(), block.size()) == "69c4e0d86a7b0430d8cdb78070b4c55a");
    REQUIRE(Aes128Ecb::decrypt_block(key, block.data()));
    REQUIRE(to_hex(block.data(), block.size()) == "00112233445566778899aabbccddeeff");
}

TEST_CASE("aes128: FIPS-197 Appendix B vector", "[aes][kat]") {
    const std::string key = key_from_hex("2b7e151628aed2a6abf7158809cf4f3c");
    auto block = from_hex("3243f6a8885a308d313198a2e0370734");
    REQUIRE(Aes128Ecb::encrypt_block(key, block.data()));
    REQUIRE(to_hex(block.data(), block.size()) == "3925841d02dc09fbdc118597196a0b32");
    REQUIRE(Aes128Ecb::decrypt_block(key, block.data()));
    REQUIRE(to_hex(block.data(), block.size()) == "3243f6a8885a308d313198a2e0370734");
}

TEST_CASE("aes128: ECB blocks are independent, which is the vendor handshake's actual requirement",
          "[aes][kat]") {
    // The handshake encrypts one padded password block twice under two keys. ECB's independence is what makes
    // the second pass meaningful, so it is asserted rather than assumed: identical inputs give identical
    // outputs, and a two-block buffer is the concatenation of the two single-block results.
    const std::string key = key_from_hex("00112233445566778899aabbccddeeff");
    auto two = from_hex("0000000000000000000000000000000000000000000000000000000000000000");
    REQUIRE(Aes128Ecb::encrypt_ecb(key, two.data(), two.size(), nullptr));
    REQUIRE(to_hex(two.data(), 16) == to_hex(two.data() + 16, 16));   // equal blocks, equal ciphertext

    auto single = from_hex("00112233445566778899aabbccddeeff");
    REQUIRE(Aes128Ecb::encrypt_block(key, single.data()));
    auto pair = from_hex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    REQUIRE(Aes128Ecb::encrypt_ecb(key, pair.data(), pair.size(), nullptr));
    REQUIRE(to_hex(pair.data(), 16) == to_hex(single.data(), 16));
    REQUIRE(to_hex(pair.data() + 16, 16) == to_hex(single.data(), 16));

    REQUIRE(Aes128Ecb::decrypt_ecb(key, pair.data(), pair.size(), nullptr));
    REQUIRE(to_hex(pair.data(), pair.size()) == "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
}

TEST_CASE("aes128: a key or buffer that is not 16-byte aligned is refused", "[aes][refusal]") {
    uint8_t block[16] = {0};
    REQUIRE_FALSE(Aes128Ecb::encrypt_block("fifteen-bytekey", block));   // 15 chars, not 16
    REQUIRE_FALSE(Aes128Ecb::decrypt_block("", block));
    REQUIRE_FALSE(Aes128Ecb::valid_key("0123456789abcdefg"));

    std::string why;
    REQUIRE_FALSE(Aes128Ecb::encrypt_ecb("0123456789abcdef", block, 20, &why));
    REQUIRE(why.find("multiple of 16") != std::string::npos);
    REQUIRE_FALSE(Aes128Ecb::encrypt_ecb("short", block, 16, &why));
    REQUIRE(why.find("16 bytes") != std::string::npos);
}

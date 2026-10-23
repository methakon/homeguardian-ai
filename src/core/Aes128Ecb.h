#pragma once

// AES-128-ECB, implemented in-repo and verified against the FIPS-197 known-answer vectors.
//
// WHY IN-REPO AND NOT OpenSSL
//   The camera codec needs one primitive: AES-128 single-block encryption under two keys in sequence. Linking
//   a TLS stack for that would make the camera path depend on OpenSSL development headers, which are NOT
//   present on the workstation that builds this project, and which a minimal ARM64 rootfs - the intended
//   Mi A3 hub target - does not carry either. One self-contained primitive removes a dependency instead of
//   adding one, and it is verified against the standard's own vectors rather than against itself.
//
// WHAT IT IS NOT
//   Not a general crypto library: ECB single-block, no modes, no padding, no streaming, no key schedule
//   caching. ECB across a whole message is a weakness in general use; it is used here ONLY because the
//   documented vendor handshake specifies exactly that, on exactly one 16-byte block. Do not reach for this
//   type to encrypt a stream.
//
// VERIFICATION
//   tests/core/test_aes128_ecb.cpp pins the FIPS-197 Appendix B and C.1 vectors in both directions, checks
//   that ECB blocks are independent, and refuses a key that is not 16 bytes.

#include <cstddef>
#include <cstdint>
#include <string>

namespace homeguardian {

class Aes128Ecb {
public:
    static constexpr size_t kBlockBytes = 16;
    static constexpr size_t kKeyBytes = 16;

    // A key that is not exactly 16 bytes cannot be accepted silently: a short key would otherwise be padded
    // with whatever follows it in memory, which is how "it works for me" and "wrong output" coexist.
    static bool valid_key(const std::string& key) { return key.size() == kKeyBytes; }

    // Encrypt/decrypt exactly one 16-byte block in place. Returns false (and leaves `block` untouched on
    // failure) when the key is the wrong length.
    static bool encrypt_block(const std::string& key, uint8_t* block);
    static bool decrypt_block(const std::string& key, uint8_t* block);

    // ECB over a whole buffer: each 16-byte block is independent, and a partial trailing block is refused
    // rather than padded (this codec never needs padding, and inventing it would diverge from the device).
    static bool encrypt_ecb(const std::string& key, uint8_t* data, size_t size, std::string* why);
    static bool decrypt_ecb(const std::string& key, uint8_t* data, size_t size, std::string* why);
};

}  // namespace homeguardian

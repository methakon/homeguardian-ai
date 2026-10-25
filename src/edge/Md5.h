// MD5 — self-contained, for the clip content digest.
//
// WHY NOT A LIBRARY
// -----------------
// The storage contract requires `content_md5` as the canonical base64 of the complete file's
// 16-byte MD5 digest. This host has no OpenSSL development headers, and the project's own
// CMakeLists states that the protocol, registry, ingest and detector layers are deliberately
// OpenSSL-free. Adding a dependency here would break that discipline for one hash, so MD5 is
// implemented directly against RFC 1321 and proved against its published test vectors.
//
// WHAT THIS IS NOT
// ----------------
// MD5 is not a security primitive and is not used as one here. It is a content integrity check
// for an upload: the storage service compares it against its own copy to detect a corrupted
// transfer. Nothing in this file authenticates anything.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hg::edge {

// 16-byte MD5 digest of a byte range.
std::vector<std::uint8_t> md5_bytes(const std::uint8_t* data, std::size_t size);

// Lowercase hex, for logs and comparison against published vectors.
std::string md5_hex(const std::uint8_t* data, std::size_t size);
inline std::string md5_hex(const std::vector<std::uint8_t>& v) {
    return md5_hex(v.data(), v.size());
}

// Canonical base64 of those 16 bytes, which is the form the storage contract specifies for
// `content_md5`. Standard alphabet, padded.
std::string base64(const std::uint8_t* data, std::size_t size);
inline std::string base64(const std::vector<std::uint8_t>& v) {
    return base64(v.data(), v.size());
}

// The digest in the contracted form: base64 of the 16-byte MD5.
std::string content_md5(const std::uint8_t* data, std::size_t size);
inline std::string content_md5(const std::vector<std::uint8_t>& v) {
    return content_md5(v.data(), v.size());
}

}  // namespace hg::edge

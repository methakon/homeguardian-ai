#include "edge/Md5.h"

#include <cstring>

namespace hg::edge {

namespace {

// RFC 1321. Little-endian on the wire, which is why the output words are unpacked byte-wise
// rather than memcpy'd.
struct Ctx {
    std::uint32_t a = 0x67452301u;
    std::uint32_t b = 0xefcdab89u;
    std::uint32_t c = 0x98badcfeu;
    std::uint32_t d = 0x10325476u;
    std::uint64_t total = 0;
    std::uint8_t buf[64] = {};
    std::size_t used = 0;
};

inline std::uint32_t rol(std::uint32_t x, std::uint32_t n) {
    return (x << n) | (x >> (32u - n));
}

void transform(Ctx& ctx, const std::uint8_t block[64]) {
    static const std::uint32_t K[64] = {
        0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
        0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
        0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
        0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
        0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
        0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
        0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
        0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u};
    static const std::uint32_t S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21};

    std::uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = static_cast<std::uint32_t>(block[i * 4]) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }

    std::uint32_t A = ctx.a, B = ctx.b, C = ctx.c, D = ctx.d;
    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t F;
        std::uint32_t g;
        if (i < 16)      { F = (B & C) | (~B & D);         g = i; }
        else if (i < 32) { F = (D & B) | (~D & C);         g = (5 * i + 1) & 15; }
        else if (i < 48) { F = B ^ C ^ D;                  g = (3 * i + 5) & 15; }
        else             { F = C ^ (B | ~D);               g = (7 * i) & 15; }
        F = F + A + K[i] + M[g];
        A = D; D = C; C = B;
        B = B + rol(F, S[i]);
    }
    ctx.a += A; ctx.b += B; ctx.c += C; ctx.d += D;
}

void update(Ctx& ctx, const std::uint8_t* data, std::size_t size) {
    ctx.total += size;
    while (size > 0) {
        const std::size_t take = (64 - ctx.used < size) ? (64 - ctx.used) : size;
        std::memcpy(ctx.buf + ctx.used, data, take);
        ctx.used += take;
        data += take;
        size -= take;
        if (ctx.used == 64) {
            transform(ctx, ctx.buf);
            ctx.used = 0;
        }
    }
}

std::vector<std::uint8_t> finish(Ctx& ctx) {
    const std::uint64_t bits = ctx.total * 8u;
    const std::uint8_t pad = 0x80;
    update(ctx, &pad, 1);
    const std::uint8_t zero = 0x00;
    while (ctx.used != 56) update(ctx, &zero, 1);
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff);
    update(ctx, len, 8);

    std::vector<std::uint8_t> out(16);
    const std::uint32_t w[4] = {ctx.a, ctx.b, ctx.c, ctx.d};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = static_cast<std::uint8_t>((w[i] >> (8 * j)) & 0xff);
        }
    }
    return out;
}

const char* kHex = "0123456789abcdef";
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::vector<std::uint8_t> md5_bytes(const std::uint8_t* data, std::size_t size) {
    Ctx ctx;
    if (data != nullptr && size > 0) update(ctx, data, size);
    return finish(ctx);
}

std::string md5_hex(const std::uint8_t* data, std::size_t size) {
    const auto d = md5_bytes(data, size);
    std::string s;
    s.reserve(32);
    for (auto b : d) {
        s.push_back(kHex[b >> 4]);
        s.push_back(kHex[b & 0x0f]);
    }
    return s;
}

std::string base64(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 0x3f]);
        out.push_back(kB64[(v >> 12) & 0x3f]);
        out.push_back(kB64[(v >> 6) & 0x3f]);
        out.push_back(kB64[v & 0x3f]);
    }
    if (i < size) {
        const bool two = (i + 1 < size);
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (two ? (static_cast<std::uint32_t>(data[i + 1]) << 8) : 0u);
        out.push_back(kB64[(v >> 18) & 0x3f]);
        out.push_back(kB64[(v >> 12) & 0x3f]);
        out.push_back(two ? kB64[(v >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

std::string content_md5(const std::uint8_t* data, std::size_t size) {
    const auto d = md5_bytes(data, size);
    return base64(d);
}

}  // namespace hg::edge

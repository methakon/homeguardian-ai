#pragma once

// V380 vendor media protocol: the wire codec.
//
// WHY THIS FILE EXISTS
//   Cameras 2 and 3 expose ONLY the vendor listeners on tcp/8800 + tcp/9800 - no RTSP, no ONVIF, and on
//   camera 2 no toggle to enable them. If their video is to reach HomeGuardian at all, it arrives over this
//   protocol, and the owner has explicitly authorized building it. This is that codec: it turns the
//   documented wire layout into bounded C++ values and back, and it is testable without a camera.
//
// PROVENANCE OF THE WIRE FACTS - read this before changing a constant
//   The layouts below are FACTS of the public protocol as documented by open re-implementations of the
//   V380/Xiongmai family (the `prsyahmi/v380` lineage and its ports). No code was copied from any of them:
//   their licensing was not verified, so nothing is vendored, and this is an independent implementation of
//   a documented interface. Facts of an interface are not a licensed work; the implementations are.
//
// TWO GENERATIONS EXIST, AND THIS CODEC ONLY SPEAKS ONE
//   Legacy (this codec): after stream login the device emits 12-byte chunk headers wrapping the media
//   payload, and that payload is the codec bitstream essentially as-is.
//   Newer: the payload is AES-encrypted and framed with a 30-byte `V380 Pro` header, and the stream login is
//   rejected. Public reverse-engineering reports the key for that generation as recoverable only from a
//   packed (Qihoo 360 Jiagu) application image at runtime - i.e. NOT from the protocol, and not by anything
//   this codec can do. So the codec does not guess an encryption it cannot know: it detects the rejection
//   and reports `unsupported_firmware_generation`. A camera of that generation is a NAMED dead end, not a
//   silent failure and not a work item that looks half-done.
//
// WHAT THIS CODEC DELIBERATELY DOES NOT DO
//   No credential is stored here, and the vendor's static key is a PARAMETER, never a constant in this
//   repository - the repository is public and a vendor decryption key does not belong in it. It is loaded
//   from the private evidence directory at runtime, under the same file-permission rule as any credential.
//   No frame is written, decoded, retained or transmitted: the codec yields bounded byte ranges and counts.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian {
namespace v380 {

// ---- wire constants (documented; do not "fix" one without a measurement) ----------------------------------

inline constexpr int32_t kCommandUserVerify = 1167;   // login
inline constexpr int32_t kCommandUserVerifyResp = 1168;
inline constexpr int32_t kCommandStreamLogin = 301;   // stream login
inline constexpr int32_t kCommandStreamLoginResp = 401;
inline constexpr int32_t kCommandStreamStart = 303;   // start the stream

inline constexpr size_t kLoginRequestBytes = 256;     // the reference sender always sends 256
inline constexpr size_t kLoginResponseBytes = 256;
inline constexpr size_t kPasswordFieldBytes = 32;     // 16-byte random key + one AES block
inline constexpr size_t kAesBlockBytes = 16;
inline constexpr size_t kRandomKeyBytes = 16;

inline constexpr size_t kChunkHeaderBytes = 12;       // legacy frame chunk header
inline constexpr uint8_t kChunkMagic = 0x7f;
inline constexpr uint8_t kChunkTypeIFrame = 0x00;
inline constexpr uint8_t kChunkTypePFrame = 0x01;
inline constexpr uint8_t kChunkTypeAudio = 0x16;
inline constexpr uint16_t kMaxChunkBytes = 500;       // documented sanity bound
inline constexpr uint16_t kKeepaliveMarker = 0x6f;    // arrives in place of a frame; benign
inline constexpr size_t kAudioPrefixBytes = 20;       // audio payload carries a 20-byte prefix

// Login result codes, each a NAMED outcome. The device returns NOTHING as text, so a boolean would throw
// away the only diagnostic the protocol provides.
enum class LoginResult {
    ok,                    // 1001
    invalid_username,      // 1011
    invalid_password,      // 1012
    invalid_device_id,     // 1018
    unexpected_command,    // not a login response at all
    short_response,        // fewer bytes than the layout needs
};

enum class StreamResult {
    ok,                              // v21 in {402, 1001}
    unsupported_firmware_generation, // v21 in {-11, -12}: the newer, encrypted generation
    refused,                         // a negative result this codec does not have a name for
    unexpected_command,
    short_response,
};

const char* to_string(LoginResult r);
const char* to_string(StreamResult r);

struct LoginRequest {
    int32_t device_id = 0;
    std::string username;
    std::string password;
};

struct LoginResponse {
    LoginResult result = LoginResult::short_response;
    int32_t command = 0;
    int32_t login_result = 0;
    uint32_t auth_ticket = 0;
    uint32_t session = 0;
};

struct StreamResponse {
    StreamResult result = StreamResult::short_response;
    int32_t command = 0;
    int32_t v21 = 0;
    uint16_t fps = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// ---- password field --------------------------------------------------------------------------------------
//
// Two-stage AES-128-ECB over the padded password: first with the vendor static key, then with a fresh random
// key, and the random key ships in the clear as the field's first 16 bytes. Reimplemented from the
// documented behaviour; `static_key` must be exactly 16 bytes and `random_key` exactly 16.
//
// REFUSED, not silently truncated: a password longer than 16 characters does not fit the 32-byte field this
// layout provides. A reference implementation memcpy's into the field without checking, which is how a
// truncated password becomes a mysterious `invalid_password` from the device. Here it is a named refusal.
bool build_password_field(const std::string& password, const std::string& static_key,
                          const std::string& random_key, std::vector<uint8_t>* out, std::string* why);

// ---- request builders -----------------------------------------------------------------------------------

// The 256-byte login request. Fails (false + reason) rather than emitting a short or oversized buffer.
bool build_login_request(const LoginRequest& request, const std::string& static_key,
                         const std::string& random_key, std::vector<uint8_t>* out, std::string* why);

// The 301 stream-login request, carrying the auth ticket from the login response.
bool build_stream_login_request(int32_t device_id, uint32_t auth_ticket, uint16_t fps, uint32_t resolution,
                                std::vector<uint8_t>* out, std::string* why);

// The 303 stream-start request, carrying the value the device returned in the 301 response.
bool build_stream_start_request(int32_t v21, std::vector<uint8_t>* out, std::string* why);

// ---- response parsers -----------------------------------------------------------------------------------

LoginResponse parse_login_response(const uint8_t* data, size_t size);
StreamResponse parse_stream_response(const uint8_t* data, size_t size);

// ---- legacy chunk reassembly -----------------------------------------------------------------------------
//
// The device sends a stream of 12-byte headers, each announcing one chunk of one logical frame. A frame is
// complete when curFrame reaches totalFrame - 1, and only then may it be handed on. This assembler is where
// a vendor protocol becomes safe to consume: it enforces the documented sanity bound, refuses a chunk whose
// index is out of range, bounds the total bytes it will hold, and NEVER emits a partial frame as if it were
// complete.
//
// It also cannot be driven into unbounded memory by a hostile or broken device: `max_frame_bytes` and
// `max_chunks_per_frame` are ceilings, and hitting either is a counted drop with a reason, not a crash.

struct ChunkHeader {
    uint8_t type = 0;
    uint16_t total_frame = 0;
    uint16_t cur_frame = 0;
    uint16_t length = 0;
};

enum class ChunkOutcome {
    need_more,          // the header (or the chunk body) is not all here yet
    keepalive,          // 0x6f: not a frame; nothing to do
    frame_complete,     // a whole frame is available
    dropped_protocol,   // sanity check failed; the chunk was dropped
    fatal_unknown,      // a header byte this codec has no rule for: the stream is not the legacy stream
};

struct AssembledFrame {
    uint8_t type = 0;             // kChunkTypeIFrame / kChunkTypePFrame / kChunkTypeAudio
    size_t payload_offset = 0;    // into the caller's buffer, where this frame's bytes begin
    size_t payload_bytes = 0;
    uint16_t chunks = 0;
};

class ChunkAssembler {
public:
    struct Config {
        size_t max_frame_bytes = 8u * 1024u * 1024u;  // ceiling on one frame
        size_t max_chunks_per_frame = 65535;
    };

    ChunkAssembler();
    explicit ChunkAssembler(Config config);

    // Parse exactly one header from `data[0..size)`. Returns kNeedMore when fewer than 12 bytes are present.
    ChunkOutcome begin_chunk(const uint8_t* data, size_t size, ChunkHeader* header);

    // Append a chunk body after begin_chunk returned a header that passed its checks.
    bool append_chunk(const ChunkHeader& header, const uint8_t* body, size_t body_size, std::string* why);

    // The complete frame, if the last appended chunk finished one.
    bool take_frame(AssembledFrame* frame);

    size_t frames_completed() const { return frames_completed_; }
    size_t chunks_dropped() const { return chunks_dropped_; }
    size_t keepalives_seen() const { return keepalives_seen_; }
    size_t frame_bytes_held() const { return buffer_.size(); }

private:
    Config cfg_;
    std::vector<uint8_t> buffer_;
    uint16_t expected_total_ = 0;
    uint16_t next_index_ = 0;
    uint16_t chunks_in_frame_ = 0;
    size_t frames_completed_ = 0;
    size_t chunks_dropped_ = 0;
    size_t keepalives_seen_ = 0;
    uint8_t last_type_ = 0;   // type of the frame being assembled
};

}  // namespace v380
}  // namespace homeguardian

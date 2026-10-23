#include "camera/v380_protocol.h"

#include "core/Aes128Ecb.h"

#include <cstring>
#include <atomic>

namespace homeguardian {
namespace v380 {
namespace {

// Explicit little-endian field access. The reference implementation casts a byte buffer to a packed struct
// (`reinterpret_cast<TCommandReq*>`), which is undefined behaviour and miscompiles under optimisation; the
// wire layout is what matters, not the struct, so it is written field by field.
constexpr bool kLittleEndianHost = true;

void write_u32(std::vector<uint8_t>* out, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) (*out)[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
}
void write_u16(std::vector<uint8_t>* out, size_t offset, uint16_t value) {
    for (size_t i = 0; i < 2; ++i) (*out)[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
}
uint32_t read_u32(const uint8_t* data, size_t offset) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[offset + i]) << (8 * i);
    return v;
}
uint16_t read_u16(const uint8_t* data, size_t offset) {
    uint16_t v = 0;
    for (size_t i = 0; i < 2; ++i) v |= static_cast<uint16_t>(data[offset + i]) << (8 * i);
    return v;
}
int32_t read_i32(const uint8_t* data, size_t offset) {
    return static_cast<int32_t>(read_u32(data, offset));
}
void write_cstr(std::vector<uint8_t>* out, size_t offset, size_t width, const std::string& value) {
    const size_t n = value.size() < width ? value.size() : width;
    std::memcpy(out->data() + offset, value.data(), n);   // the rest stays zero-filled
}

bool aes128_ecb_encrypt_in_place(const std::string& key, uint8_t* data, size_t size, std::string* why) {
    // The in-repo cipher, not a TLS stack: see src/core/Aes128Ecb.h for why (the target is a minimal ARM64
    // rootfs, and OpenSSL headers are not present on the machine that builds this project either).
    return homeguardian::Aes128Ecb::encrypt_ecb(key, data, size, why);
}

}  // namespace

const char* to_string(LoginResult r) {
    switch (r) {
        case LoginResult::ok: return "ok";
        case LoginResult::invalid_username: return "invalid_username";
        case LoginResult::invalid_password: return "invalid_password";
        case LoginResult::invalid_device_id: return "invalid_device_id";
        case LoginResult::unexpected_command: return "unexpected_command";
        case LoginResult::short_response: return "short_response";
    }
    return "unknown";
}

const char* to_string(StreamResult r) {
    switch (r) {
        case StreamResult::ok: return "ok";
        case StreamResult::unsupported_firmware_generation: return "unsupported_firmware_generation";
        case StreamResult::refused: return "refused";
        case StreamResult::unexpected_command: return "unexpected_command";
        case StreamResult::short_response: return "short_response";
    }
    return "unknown";
}

bool build_password_field(const std::string& password, const std::string& static_key,
                          const std::string& random_key, std::vector<uint8_t>* out, std::string* why) {
    if (out == nullptr || why == nullptr) return false;
    out->clear();
    why->clear();
    if (static_key.size() != kAesBlockBytes) {
        *why = "the vendor static key must be exactly 16 bytes (loaded from the private evidence directory)";
        return false;
    }
    if (random_key.size() != kRandomKeyBytes) {
        *why = "the random key must be exactly 16 bytes";
        return false;
    }
    // An EMPTY password is a legitimate input for this family, not a mistake: the QR-code setup path lets the
    // owner leave the device password unset, and the field is then a fresh random key plus one encrypted
    // all-zero block. The caller must still pass it deliberately (an absent argument is not an empty one),
    // but the codec does not refuse it. Refusing here was my own invention and would have blocked the only
    // credential this device actually has.
    // 16 bytes of clear random key + one AES block == the 32-byte field. A longer password needs more
    // blocks than the field can hold, so it is refused HERE, loudly, instead of being truncated into a
    // device-side `invalid_password` that would look like a wrong credential.
    if (password.size() > kPasswordFieldBytes - kRandomKeyBytes) {
        *why = "password is longer than the 16 characters this legacy field can hold; it cannot be sent " +
               std::to_string(kPasswordFieldBytes - kRandomKeyBytes) + " bytes of key + block";
        return false;
    }

    const size_t pad = kAesBlockBytes - (password.size() % kAesBlockBytes);
    std::vector<uint8_t> block(password.size() + pad, 0);
    std::memcpy(block.data(), password.data(), password.size());

    std::string stage_why;
    if (!aes128_ecb_encrypt_in_place(static_key, block.data(), block.size(), &stage_why)) {
        *why = "stage 1 (static key) failed: " + stage_why;
        return false;
    }
    if (!aes128_ecb_encrypt_in_place(random_key, block.data(), block.size(), &stage_why)) {
        *why = "stage 2 (random key) failed: " + stage_why;
        return false;
    }

    out->resize(kPasswordFieldBytes, 0);
    std::memcpy(out->data(), random_key.data(), kRandomKeyBytes);
    std::memcpy(out->data() + kRandomKeyBytes, block.data(), block.size());
    return true;
}

bool build_login_request(const LoginRequest& request, const std::string& static_key,
                         const std::string& random_key, std::vector<uint8_t>* out, std::string* why) {
    if (out == nullptr || why == nullptr) return false;
    why->clear();
    if (request.device_id <= 0) {
        *why = "a login request needs a positive device id";
        return false;
    }
    if (request.username.size() > 32) {
        *why = "the username field is 32 bytes";
        return false;
    }
    std::vector<uint8_t> field;
    if (!build_password_field(request.password, static_key, random_key, &field, why)) return false;

    out->assign(kLoginRequestBytes, 0);
    write_u32(out, 0, static_cast<uint32_t>(kCommandUserVerify));
    write_u32(out, 4, 1022u);   // unknown1: the "us" variant of the login command
    (*out)[8] = 2;              // unknown2
    write_u32(out, 9, 1u);      // unknown3
    write_u32(out, 13, static_cast<uint32_t>(request.device_id));
    write_cstr(out, 49, 32, request.username);
    std::memcpy(out->data() + 81, field.data(), field.size());
    (void)kLittleEndianHost;
    return true;
}

bool build_stream_login_request(int32_t device_id, uint32_t auth_ticket, uint16_t fps, uint32_t resolution,
                                std::vector<uint8_t>* out, std::string* why) {
    if (out == nullptr || why == nullptr) return false;
    why->clear();
    if (device_id <= 0) {
        *why = "a stream login needs a positive device id";
        return false;
    }
    if (fps < 10 || fps > 20) {
        *why = "the device documents fps in [10,20]; an invalid value silently falls back to 13";
        return false;
    }
    if (resolution > 1) {
        *why = "resolution is 0 (low) or 1 (high)";
        return false;
    }
    out->assign(kLoginRequestBytes, 0);
    write_u32(out, 0, static_cast<uint32_t>(kCommandStreamLogin));
    write_u32(out, 4, static_cast<uint32_t>(device_id));
    write_u32(out, 8, 0u);                       // unknown1
    write_u16(out, 12, fps);
    write_u32(out, 14, auth_ticket);
    write_u32(out, 22, 4096u + 1u);              // unknown4, as the documented exchange uses it
    write_u32(out, 26, resolution);
    return true;
}

bool build_stream_start_request(int32_t v21, std::vector<uint8_t>* out, std::string* why) {
    if (out == nullptr || why == nullptr) return false;
    why->clear();
    out->assign(kLoginRequestBytes, 0);
    write_u32(out, 0, static_cast<uint32_t>(kCommandStreamStart));
    write_u32(out, 4, static_cast<uint32_t>(v21));   // streamStart.unknown1 carries the returned value
    return true;
}

LoginResponse parse_login_response(const uint8_t* data, size_t size) {
    LoginResponse r;
    if (data == nullptr || size < 21) {   // command + result + resultValue + version + authTicket + session
        r.result = LoginResult::short_response;
        return r;
    }
    r.command = read_i32(data, 0);
    r.login_result = read_i32(data, 4);
    r.auth_ticket = read_u32(data, 13);
    r.session = read_u32(data, 17);
    if (r.command != kCommandUserVerifyResp) {
        r.result = LoginResult::unexpected_command;
        return r;
    }
    switch (r.login_result) {
        case 1001: r.result = LoginResult::ok; break;
        case 1011: r.result = LoginResult::invalid_username; break;
        case 1012: r.result = LoginResult::invalid_password; break;
        case 1018: r.result = LoginResult::invalid_device_id; break;
        default:   r.result = LoginResult::invalid_password; break;   // named as a refusal, not a success
    }
    return r;
}

StreamResponse parse_stream_response(const uint8_t* data, size_t size) {
    StreamResponse r;
    if (data == nullptr || size < 18) {   // command + v21 + fps + width + height
        r.result = StreamResult::short_response;
        return r;
    }
    r.command = read_i32(data, 0);
    r.v21 = read_i32(data, 4);
    r.fps = read_u16(data, 8);
    r.width = read_u32(data, 10);
    r.height = read_u32(data, 14);
    if (r.command != kCommandStreamLoginResp) {
        r.result = StreamResult::unexpected_command;
        return r;
    }
    if (r.v21 == 402 || r.v21 == 1001) {
        r.result = StreamResult::ok;
    } else if (r.v21 == -11 || r.v21 == -12) {
        // THE generation boundary. -11/-12 is the newer encrypted-generation stream login: this codec has no
        // key for it and cannot get one from the protocol, so the outcome is named rather than retried.
        r.result = StreamResult::unsupported_firmware_generation;
    } else {
        r.result = StreamResult::refused;
    }
    return r;
}

namespace {
std::atomic<size_t>& aggregate_bytes_held_counter() {
    static std::atomic<size_t> counter{0};
    return counter;
}
}  // namespace

ChunkAssembler::ChunkAssembler() = default;
ChunkAssembler::ChunkAssembler(Config config) : cfg_(config) {}

ChunkAssembler::~ChunkAssembler() {
    if (!buffer_.empty()) {
        aggregate_bytes_held_counter().fetch_sub(buffer_.size(), std::memory_order_relaxed);
    }
}

ChunkAssembler::ChunkAssembler(ChunkAssembler&& other) noexcept
    : cfg_(other.cfg_),
      buffer_(std::move(other.buffer_)),
      expected_total_(other.expected_total_),
      next_index_(other.next_index_),
      chunks_in_frame_(other.chunks_in_frame_),
      frames_completed_(other.frames_completed_),
      chunks_dropped_(other.chunks_dropped_),
      keepalives_seen_(other.keepalives_seen_),
      last_type_(other.last_type_) {
    other.expected_total_ = 0;
    other.next_index_ = 0;
    other.chunks_in_frame_ = 0;
    other.last_type_ = 0;
}

ChunkAssembler& ChunkAssembler::operator=(ChunkAssembler&& other) noexcept {
    if (this != &other) {
        if (!buffer_.empty()) {
            aggregate_bytes_held_counter().fetch_sub(buffer_.size(), std::memory_order_relaxed);
        }
        cfg_ = other.cfg_;
        expected_total_ = other.expected_total_;
        next_index_ = other.next_index_;
        chunks_in_frame_ = other.chunks_in_frame_;
        last_type_ = other.last_type_;
        frames_completed_ = other.frames_completed_;
        chunks_dropped_ = other.chunks_dropped_;
        keepalives_seen_ = other.keepalives_seen_;
        buffer_ = std::move(other.buffer_);

        other.expected_total_ = 0;
        other.next_index_ = 0;
        other.chunks_in_frame_ = 0;
        other.last_type_ = 0;
    }
    return *this;
}

size_t ChunkAssembler::aggregate_frame_bytes_held() {
    return aggregate_bytes_held_counter().load(std::memory_order_relaxed);
}

ChunkOutcome ChunkAssembler::begin_chunk(const uint8_t* data, size_t size, ChunkHeader* header) {
    if (header == nullptr) return ChunkOutcome::dropped_protocol;
    if (data == nullptr || size < kChunkHeaderBytes) return ChunkOutcome::need_more;

    if (data[0] == static_cast<uint8_t>(kKeepaliveMarker)) {
        ++keepalives_seen_;
        return ChunkOutcome::keepalive;
    }
    if (data[0] != kChunkMagic) return ChunkOutcome::fatal_unknown;

    header->type = data[1];
    header->total_frame = read_u16(data, 3);
    header->cur_frame = read_u16(data, 5);
    header->length = read_u16(data, 7);

    const bool known_type = header->type == kChunkTypeIFrame || header->type == kChunkTypePFrame ||
                            header->type == kChunkTypeAudio;
    if (!known_type || header->total_frame == 0 || header->cur_frame >= header->total_frame ||
        header->length > kMaxChunkBytes || header->length == 0) {
        ++chunks_dropped_;
        return ChunkOutcome::dropped_protocol;
    }
    return ChunkOutcome::need_more;   // header is valid; the caller now supplies the body
}

bool ChunkAssembler::append_chunk(const ChunkHeader& header, const uint8_t* body, size_t body_size,
                                  std::string* why) {
    // `why` is OPTIONAL: a caller that only needs the boolean must not be refused a result. The first version
    // returned false when the caller passed nullptr, which the protocol tests caught immediately.
    if (why != nullptr) why->clear();
    if (body == nullptr || body_size != header.length) {
        if (why != nullptr) *why = "chunk body length does not match the header";
        ++chunks_dropped_;
        return false;
    }

    auto discard_current_frame = [this]() {
        if (!buffer_.empty()) {
            aggregate_bytes_held_counter().fetch_sub(buffer_.size(), std::memory_order_relaxed);
            buffer_.clear();
        }
        expected_total_ = 0;
        last_type_ = 0;
        next_index_ = 0;
        chunks_in_frame_ = 0;
    };

    if (buffer_.empty()) {
        expected_total_ = header.total_frame;
        next_index_ = 0;
        chunks_in_frame_ = 0;
        last_type_ = header.type;
    } else if (header.type != last_type_) {
        // A frame whose chunks disagree about their own type is not a frame this codec can assemble.
        if (why != nullptr) *why = "chunk type changed mid-frame";
        ++chunks_dropped_;
        discard_current_frame();
        return false;
    }
    if (header.cur_frame != next_index_) {
        if (why != nullptr) {
            *why = "out-of-order chunk: expected index " + std::to_string(next_index_) + ", got " +
                   std::to_string(header.cur_frame);
        }
        ++chunks_dropped_;
        discard_current_frame();
        return false;
    }
    if (buffer_.size() + body_size > cfg_.max_frame_bytes) {
        if (why != nullptr) {
            *why = "frame exceeds the configured ceiling of " + std::to_string(cfg_.max_frame_bytes) + " bytes";
        }
        ++chunks_dropped_;
        discard_current_frame();
        return false;
    }
    if (chunks_in_frame_ + 1 > cfg_.max_chunks_per_frame) {
        if (why != nullptr) *why = "frame exceeds the configured chunk ceiling";
        ++chunks_dropped_;
        discard_current_frame();
        return false;
    }
    if (aggregate_bytes_held_counter().load(std::memory_order_relaxed) + body_size > cfg_.max_aggregate_bytes) {
        if (why != nullptr) *why = "aggregate V380 frame reassembly memory ceiling exceeded";
        ++chunks_dropped_;
        discard_current_frame();
        return false;
    }

    buffer_.insert(buffer_.end(), body, body + body_size);
    aggregate_bytes_held_counter().fetch_add(body_size, std::memory_order_relaxed);
    ++chunks_in_frame_;
    ++next_index_;
    return true;
}

bool ChunkAssembler::take_frame(AssembledFrame* frame) {
    if (frame == nullptr) return false;
    if (expected_total_ == 0 || next_index_ == 0 || next_index_ < expected_total_) return false;

    const uint8_t type = chunks_in_frame_ > 0 ? last_type_ : 0;   // set on the final chunk below
    frame->type = type;
    frame->chunks = chunks_in_frame_;
    // Audio carries a fixed 20-byte prefix this codec does not interpret; the media layer strips it. The
    // frame is reported with its offset so nothing is silently discarded here.
    frame->payload_offset = (type == kChunkTypeAudio && buffer_.size() >= kAudioPrefixBytes)
                                ? kAudioPrefixBytes
                                : 0;
    frame->payload_bytes = buffer_.size() - frame->payload_offset;
    ++frames_completed_;
    if (!buffer_.empty()) {
        aggregate_bytes_held_counter().fetch_sub(buffer_.size(), std::memory_order_relaxed);
        buffer_.clear();
    }
    expected_total_ = 0;
    next_index_ = 0;
    chunks_in_frame_ = 0;
    last_type_ = 0;
    return true;
}

}  // namespace v380
}  // namespace homeguardian

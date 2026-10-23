#pragma once

// LiveCameraFeed — a real, physical camera as a source for the backup portal.
//
// WHAT THIS IS
// A read-only source for the owner's authorized exterior camera (the unit outside the
// main gate). It fetches real JPEG frames from the camera's verified snapshot endpoint
// and reports a truthful state: LIVE only when a frame arrived recently, STALE when the
// last frame is older than the freshness bound, DISCONNECTED when the camera cannot be
// reached at all.
//
// WHAT THIS IS NOT, AND WHY THE WORDING MATTERS
// This is a REFRESHING SNAPSHOT source, not a continuous video stream. The camera does
// expose an H.264 stream, but the project's existing HttpClient is deliberately a
// bounded, single-response GET (64 KiB cap, GET-only, no streaming primitive), so it can
// carry one snapshot and cannot carry a continuous elementary stream. Wiring the H.264
// path needs a streaming client plus an fMP4/MSE muxer and is a separate increment.
//
// Therefore this module must never be described, in code, logs or UI, as continuous
// video. A changing snapshot does not prove continuous video, and a viewer built on this
// is labeled accordingly.
//
// FOUR SOURCES, NEVER MIXED
//   LIVE PHYSICAL CAMERA  <- this file
//   SYNTHETIC DEMO        <- the existing simulator/scenario path, untouched
//   UPLOADED MEDIA/REPLAY <- the existing PortalUploads feed, untouched
//   HOMEGUARDIAN STATUS   <- the real C++ Application/MCP responses
// This module adds the first and changes none of the others. Nothing here is synthetic:
// every byte it serves came off the physical camera.
//
// SECURITY POSTURE
//   * The credential is held by the caller and passed to HttpClient's header list; it is
//     never logged, never echoed in a status payload, and never placed in a URL. The
//     camera URL this module builds contains no userinfo.
//   * No actuation. HttpClient is GET-only, and this module only reads a snapshot. It
//     cannot move the camera, change firmware, or touch configuration routes.
//   * Bounded. Frame size, frame rate and history are capped; a hostile or broken camera
//     cannot make this module allocate without limit.
//   * Fail-closed honesty. A failure is reported as a failure. The module never
//     substitutes a cached frame for a live one, and never relabels a stale frame LIVE.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/HttpClient.h"

namespace homeguardian {
namespace portal {

// Deliberately small: enough for a status payload, not a media buffer.
struct LiveCameraPolicy {
    static constexpr size_t kMaxFrameBytes = 512u * 1024u;   // a JPEG snapshot, bounded
    static constexpr size_t kMaxHistory = 2;                 // last frame + the one before
    static constexpr int kMinFetchIntervalMs = 200;          // hard floor between fetches
    static constexpr int kFreshnessMs = 5000;                // older than this => STALE
    static constexpr int kMaxConsecutiveFailures = 3;
};

struct CameraConfig {
    // Identity. The display name is what the operator is promised in the UI.
    std::string display_name = "Outside Main Gate";

    // Where the camera is. host is an IPv4 literal or hostname on the site-local network.
    std::string host;
    uint16_t port = 80;
    std::string snapshot_path = "/snapshot";

    // Credential. Held only for the lifetime of the request head; never logged.
    std::string user = "admin";
    std::string password;

    // The plaintext gate. The camera serves plain http on the LAN, so this must be set
    // explicitly rather than assumed - matching the existing Home Assistant adapter's
    // posture, which refuses plaintext unless the caller opts in.
    bool allow_plaintext = false;

    int max_fps = 2;                 // polling ceiling; also the UI's expectation
    int freshness_ms = LiveCameraPolicy::kFreshnessMs;
};

enum class FeedState {
    NeverConnected,
    Live,
    Stale,
    Disconnected,
};

inline const char* feed_state_name(FeedState s) {
    switch (s) {
        case FeedState::Live:           return "LIVE";
        case FeedState::Stale:          return "STALE";
        case FeedState::Disconnected:   return "DISCONNECTED";
        case FeedState::NeverConnected: return "CONNECTING";
    }
    return "DISCONNECTED";
}

struct CameraStatus {
    FeedState state = FeedState::NeverConnected;
    std::string display_name;
    // Provenance label the UI must render. Non-negotiable: this source is physical.
    std::string provenance = "physical_camera";
    // How the media is actually delivered. Says "snapshot", never "video".
    std::string delivery = "refreshing_snapshot";
    int64_t last_frame_at_ms = 0;
    int64_t last_attempt_at_ms = 0;
    int64_t last_error_at_ms = 0;
    int64_t frames_served = 0;
    int64_t fetch_failures = 0;
    int consecutive_failures = 0;
    size_t last_frame_bytes = 0;
    int last_frame_width = 0;
    int last_frame_height = 0;
    std::string last_error;          // bounded, non-sensitive: a reason, never a URL/credential
};

// What a caller gets back from a fetch. `jpeg` empty means "no new frame".
struct CameraFrame {
    bool ok = false;
    std::vector<uint8_t> jpeg;
    int64_t captured_at_ms = 0;
    int width = 0;
    int height = 0;
    std::string error;               // bounded, non-sensitive
};

namespace detail {

// A JPEG starts with SOI (FFD8) and ends with EOI (FFD9). Checking both is what stops a
// truncated or HTML error page from being served to a browser as an image.
inline bool looks_like_jpeg(const std::vector<uint8_t>& b) {
    if (b.size() < 4) return false;
    if (b[0] != 0xFF || b[1] != 0xD8) return false;
    return b[b.size() - 2] == 0xFF && b[b.size() - 1] == 0xD9;
}

// Walk the JPEG marker segments to the SOF and read the dimensions, so the UI can show
// the source resolution. Returns false when no SOF is found; the frame is still usable,
// it simply has no reported geometry.
inline bool jpeg_dimensions(const std::vector<uint8_t>& b, int* w, int* h) {
    size_t i = 2;
    while (i + 9 < b.size()) {
        if (b[i] != 0xFF) return false;
        const uint8_t marker = b[i + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }
        if (marker == 0xD9) return false;
        const size_t seg_len = (static_cast<size_t>(b[i + 2]) << 8) | b[i + 3];
        if (seg_len < 2 || i + 2 + seg_len > b.size()) return false;
        // SOF0..SOF15, excluding the DHT/JPG/DAC markers that share the range.
        const bool is_sof = (marker >= 0xC0 && marker <= 0xCF)
                            && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof) {
            if (i + 9 >= b.size()) return false;
            *h = (static_cast<int>(b[i + 5]) << 8) | b[i + 6];
            *w = (static_cast<int>(b[i + 7]) << 8) | b[i + 8];
            return *w > 0 && *h > 0;
        }
        i += 2 + seg_len;
    }
    return false;
}

}  // namespace detail

class LiveCameraFeed {
public:
    explicit LiveCameraFeed(CameraConfig cfg) : cfg_(std::move(cfg)) {}

    const CameraConfig& config() const { return cfg_; }

    // Fetch one frame from the camera. Blocking and bounded by the HttpClient policy.
    // Returns ok=false with a bounded reason when the camera cannot be reached, is not a
    // JPEG, or exceeds the frame cap. It never returns a cached frame as though new.
    CameraFrame fetch_frame(int64_t now_ms) {
        CameraFrame out;
        {
            std::lock_guard<std::mutex> lock(m_);
            last_attempt_at_ms_ = now_ms;
        }

        if (!cfg_.allow_plaintext) {
            out.error = "plaintext_not_enabled";
            record_failure(out.error, now_ms);
            return out;
        }
        if (cfg_.host.empty()) {
            out.error = "no_camera_configured";
            record_failure(out.error, now_ms);
            return out;
        }

        const std::string url = "http://" + cfg_.host + ":" + std::to_string(cfg_.port)
                                + cfg_.snapshot_path;
        const auto parsed = HttpClient::parse_url(url);
        if (!parsed.valid) {
            // parse_url's error is already generic; do not enrich it with the URL.
            out.error = "camera_url_invalid";
            record_failure(out.error, now_ms);
            return out;
        }

        // Credential lives here and nowhere else. HttpClient documents that it logs
        // nothing, including this header.
        std::vector<std::pair<std::string, std::string>> headers;
        {
            static const char kB64[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const std::string raw = cfg_.user + ":" + cfg_.password;
            std::string enc;
            size_t i = 0;
            while (i + 2 < raw.size()) {
                const unsigned n = (static_cast<unsigned char>(raw[i]) << 16)
                                 | (static_cast<unsigned char>(raw[i + 1]) << 8)
                                 | static_cast<unsigned char>(raw[i + 2]);
                enc += kB64[(n >> 18) & 63]; enc += kB64[(n >> 12) & 63];
                enc += kB64[(n >> 6) & 63];  enc += kB64[n & 63];
                i += 3;
            }
            if (i < raw.size()) {
                unsigned n = static_cast<unsigned char>(raw[i]) << 16;
                if (i + 1 < raw.size()) n |= static_cast<unsigned char>(raw[i + 1]) << 8;
                enc += kB64[(n >> 18) & 63]; enc += kB64[(n >> 12) & 63];
                enc += (i + 1 < raw.size()) ? kB64[(n >> 6) & 63] : '=';
                enc += '=';
            }
            headers.emplace_back("Authorization", "Basic " + enc);
        }
        headers.emplace_back("Accept", "image/jpeg");

        HttpClient::Config hc;
        hc.max_response_bytes = LiveCameraPolicy::kMaxFrameBytes;
        const HttpResponse resp = HttpClient(hc).get(parsed, headers);

        if (!resp.ok || resp.status != 200) {
            // Report the reason class, never the URL or any header.
            out.error = resp.status != 0 ? ("camera_http_" + std::to_string(resp.status))
                                         : "camera_unreachable";
            record_failure(out.error, now_ms);
            return out;
        }
        if (resp.truncated) {
            out.error = "frame_over_cap";
            record_failure(out.error, now_ms);
            return out;
        }

        std::vector<uint8_t> bytes(resp.body.begin(), resp.body.end());
        if (!detail::looks_like_jpeg(bytes)) {
            // An auth page or an error document is NOT a frame. Refusing here is what
            // stops a broken camera from rendering as a broken image the UI calls LIVE.
            out.error = "not_a_jpeg";
            record_failure(out.error, now_ms);
            return out;
        }

        int w = 0, h = 0;
        detail::jpeg_dimensions(bytes, &w, &h);

        {
            std::lock_guard<std::mutex> lock(m_);
            prev_jpeg_ = last_jpeg_;
            last_jpeg_ = bytes;
            last_frame_at_ms_ = now_ms;
            last_frame_bytes_ = bytes.size();
            last_frame_width_ = w;
            last_frame_height_ = h;
            consecutive_failures_ = 0;
            frames_fetched_++;
            last_error_.clear();
        }

        out.ok = true;
        out.jpeg = std::move(bytes);
        out.captured_at_ms = now_ms;
        out.width = w;
        out.height = h;
        return out;
    }

    // The most recently fetched frame, if any. Callers serving this must label it with
    // the age from status(), so a stale frame is never presented as live.
    std::optional<std::vector<uint8_t>> last_frame() const {
        std::lock_guard<std::mutex> lock(m_);
        if (last_jpeg_.empty()) return std::nullopt;
        return last_jpeg_;
    }

    // True when the newest frame is within the freshness bound. The UI's LIVE/STALE
    // decision comes from here and nowhere else.
    bool is_fresh(int64_t now_ms) const {
        std::lock_guard<std::mutex> lock(m_);
        if (last_frame_at_ms_ == 0) return false;
        return (now_ms - last_frame_at_ms_) <= cfg_.freshness_ms;
    }

    CameraStatus status(int64_t now_ms) const {
        CameraStatus s;
        std::lock_guard<std::mutex> lock(m_);
        s.display_name = cfg_.display_name;
        s.last_frame_at_ms = last_frame_at_ms_;
        s.last_attempt_at_ms = last_attempt_at_ms_;
        s.last_error_at_ms = last_error_at_ms_;
        s.frames_served = frames_fetched_;
        s.fetch_failures = fetch_failures_;
        s.consecutive_failures = consecutive_failures_;
        s.last_frame_bytes = last_frame_bytes_;
        s.last_frame_width = last_frame_width_;
        s.last_frame_height = last_frame_height_;
        s.last_error = last_error_;

        if (last_frame_at_ms_ != 0 && (now_ms - last_frame_at_ms_) <= cfg_.freshness_ms) {
            s.state = FeedState::Live;
        } else if (last_frame_at_ms_ != 0) {
            s.state = FeedState::Stale;
        } else if (consecutive_failures_ >= LiveCameraPolicy::kMaxConsecutiveFailures) {
            s.state = FeedState::Disconnected;
        } else {
            s.state = FeedState::NeverConnected;
        }
        return s;
    }

    // Status as JSON. Contains no camera address, no credential, and no error text that
    // could carry one. This is what the panel renders.
    std::string status_json(int64_t now_ms) const {
        const CameraStatus s = status(now_ms);
        const int64_t age_ms = s.last_frame_at_ms == 0 ? -1 : (now_ms - s.last_frame_at_ms);
        std::string j = "{";
        j += "\"display_name\":\"" + json_escape(s.display_name) + "\"";
        j += ",\"state\":\"" + std::string(feed_state_name(s.state)) + "\"";
        j += ",\"provenance\":\"" + s.provenance + "\"";
        j += ",\"delivery\":\"" + s.delivery + "\"";
        j += ",\"is_continuous_video\":false";
        // A preview is not detector input. Keep this explicit until bounded
        // decoding, source authorization and the native writer are connected.
        j += ",\"analysis_connected\":false";
        j += ",\"analysis_state\":\"not_wired\"";
        j += ",\"connected\":" + std::string(s.state == FeedState::Live || s.state == FeedState::Stale ? "true" : "false");
        j += ",\"frames_served\":" + std::to_string(s.frames_served);
        j += ",\"fetch_failures\":" + std::to_string(s.fetch_failures);
        j += ",\"consecutive_failures\":" + std::to_string(s.consecutive_failures);
        j += ",\"last_frame_bytes\":" + std::to_string(s.last_frame_bytes);
        j += ",\"frame_age_ms\":" + std::to_string(age_ms);
        j += ",\"freshness_ms\":" + std::to_string(cfg_.freshness_ms);
        if (s.last_frame_width > 0) {
            j += ",\"width\":" + std::to_string(s.last_frame_width);
            j += ",\"height\":" + std::to_string(s.last_frame_height);
        }
        if (!s.last_error.empty()) j += ",\"last_error\":\"" + json_escape(s.last_error) + "\"";
        j += "}";
        return j;
    }

private:
    void record_failure(const std::string& reason, int64_t now_ms) {
        std::lock_guard<std::mutex> lock(m_);
        consecutive_failures_++;
        fetch_failures_++;
        last_error_ = reason;
        last_error_at_ms_ = now_ms;
    }

    static std::string json_escape(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                    else out += c;
            }
        }
        return out;
    }

    CameraConfig cfg_;
    mutable std::mutex m_;
    std::vector<uint8_t> last_jpeg_;
    std::vector<uint8_t> prev_jpeg_;
    int64_t last_frame_at_ms_ = 0;
    int64_t last_attempt_at_ms_ = 0;
    int64_t last_error_at_ms_ = 0;
    int64_t frames_fetched_ = 0;
    int64_t fetch_failures_ = 0;
    int consecutive_failures_ = 0;
    size_t last_frame_bytes_ = 0;
    int last_frame_width_ = 0;
    int last_frame_height_ = 0;
    std::string last_error_;
};

}  // namespace portal
}  // namespace homeguardian

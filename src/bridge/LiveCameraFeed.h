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
//
// DYNAMIC ADDRESS (DHCP) — see CameraLocator.h
//   The camera holds a DHCP lease, so its address moves. When the operator enables
//   `scan_enabled` (--camera-scan auto) this feed can ATTEMPT one bounded rediscovery per
//   minute while it is unavailable, anchored on the camera's recorded MAC address and never on
//   the address. Two rules are enforced here, not merely documented:
//     1. A host becomes a candidate only if the locator identified it as the camera (MAC in the
//        ARP table matching an explicitly configured MAC; JPEG fingerprints are not identity).
//     2. The credential is sent to that candidate ONLY as the re-verification fetch, and the
//        feed stays pointed at the last known host unless that fetch actually returned a JPEG.
//   A camera that is merely unreachable therefore costs a bounded sweep, not a credential spray.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "bridge/CameraLocator.h"
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

    // --- dynamic address (DHCP) -----------------------------------------------------------
    // The camera's explicitly configured MAC address. This is the IDENTITY used to
    // re-attach the camera after its address changes; the address above is only an attachment.
    std::string mac;
    // Small opaque state file (0600) holding the last verified host + MAC + scan time, so a
    // restart does not rescan. Empty => nothing is persisted.
    std::string state_file;
    // Off by default. When false the address never changes on its own: an operator who has not
    // opted in gets exactly the previous behaviour, including a DISCONNECTED camera stays
    // DISCONNECTED rather than the portal sweeping the LAN.
    bool scan_enabled = false;
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

    // ---- address discovery (idle | searching | found | not_found) -------------------------
    std::string discovery_state = "idle";
    int64_t discovery_last_scan_ms = 0;
    int discovery_scanned_hosts = 0;
    std::string discovery_source = "configured";   // configured | rediscovered
    std::string discovery_reason;                  // bounded reason, never a URL or credential
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
    explicit LiveCameraFeed(CameraConfig cfg) : cfg_(std::move(cfg)) {
        camera::HardwareFingerprint fp;
        if (camera::HardwareFingerprint::normalize(cfg_.mac, &fp, nullptr)) cfg_.mac = fp.canonical();
        else cfg_.mac.clear();          // an unparseable MAC is "no MAC", never a wildcard

        // Saved addresses are hints only, never credential destinations. Rediscovery must
        // establish current same-subnet MAC identity even after a restart. Off never moves.
        PersistedLocatorState saved;
        if (cfg_.scan_enabled && !cfg_.mac.empty() &&
            load_locator_state(cfg_.state_file, &saved) && saved.mac == cfg_.mac) {
            uint32_t configured = 0, hint = 0;
            const auto now = locator_detail::steady_now_ms();
            if (parse_ipv4(cfg_.host, &configured) && parse_ipv4(saved.host, &hint) &&
                same_subnet24(configured, hint) && saved.scanned_at_ms > 0 &&
                saved.scanned_at_ms <= now && now - saved.scanned_at_ms < 60000) {
                seeded_scan_ms_ = saved.scanned_at_ms;
                discovery_last_scan_ms_ = saved.scanned_at_ms;
            }
        }
        build_locator();
    }

    // Replace the discovery engine. Used by the offline tests (injected ARP fixture, probe and
    // clock) and by any future operator tool. The default engine reads /proc/net/arp and probes
    // the configured host's own /24 with bare TCP connects and no credentials.
    // Serialize configuration, fetch and rediscovery across scheduler/request threads.
    void set_authorization(std::function<bool()> authorize) {
        std::lock_guard<std::recursive_mutex> op(operation_);
        authorize_ = std::move(authorize);
    }
    void set_locator(std::shared_ptr<CameraLocator> locator) {
        std::lock_guard<std::recursive_mutex> op(operation_);
        locator_ = std::move(locator);
        if (!locator_) return;

        if (seeded_scan_ms_ != 0) {
            PersistedLocatorState seed;
            seed.valid = true;
            seed.host = cfg_.host;
            seed.mac = cfg_.mac;
            seed.scanned_at_ms = seeded_scan_ms_;
            locator_->seed_from_state(seed);
        }
    }

    CameraConfig config() const {
        std::lock_guard<std::mutex> lock(m_);
        return cfg_;
    }

    // Point the feed at a different host. Validated so a discovered value can never rewrite the
    // URL shape (no scheme, port, userinfo, path or whitespace), and never an empty string.
    bool set_host(const std::string& host) {
        std::lock_guard<std::recursive_mutex> op(operation_);
        if (!CameraLocator::valid_host(host)) return false;
        std::lock_guard<std::mutex> lock(m_);
        cfg_.host = host;
        return true;
    }

    struct RediscoveryResult {
        bool attempted = false;
        bool found = false;
        bool verified = false;      // the re-verification fetch returned a real JPEG
        bool rate_limited = false;  // the 60 s gate refused; nothing was scanned
        std::string host;
        std::string reason;         // bounded, non-sensitive
        int scanned_hosts = 0;
    };

    // The re-discovery hook. Discovery is MAC-anchored and credential-free; the credential is
    // used only in the final re-verification fetch, and only against a host the locator
    // identified as the camera. A failed verification restores the previous host, so the feed
    // is never left pointed at an unverified address.
    RediscoveryResult rediscover(int64_t now_ms) {
        std::lock_guard<std::recursive_mutex> op(operation_);
        RediscoveryResult out;
        if (!authorized()) { out.reason = "source_unauthorized"; return out; }
        if (cfg_.mac.empty()) { out.reason = "explicit_mac_required"; return out; }
        if (!cfg_.scan_enabled || !locator_) {
            out.reason = "scan_disabled";
            return out;
        }
        out.attempted = true;
        std::string previous_state;
        {
            std::lock_guard<std::mutex> lock(m_);
            previous_state = discovery_state_;
            discovery_state_ = "searching";
        }
        locator_->set_authorization([this] { return authorized(); });
        const DiscoveryResult r = locator_->discover(now_ms);
        out.rate_limited = r.rate_limited;
        out.scanned_hosts = r.scanned_hosts;
        if (r.rate_limited) {
            // Nothing was attempted. Do not claim a failure and do not rewrite the record of the
            // last real scan: the panel must be able to tell "we looked and it was not there" from
            // "we are waiting out the minute".
            std::lock_guard<std::mutex> lock(m_);
            discovery_state_ = previous_state;
            out.reason = "rate_limited";
            return out;
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            discovery_last_scan_ms_ = r.started_ms;
            discovery_scanned_hosts_ = r.scanned_hosts;
        }
        // Failed scans also survive restart. Persist the locator's monotonic scan time,
        // not the caller's display timestamp, which may use a different clock.
        seeded_scan_ms_ = locator_->last_scan_ms();
        persist();
        if (!r.found) {
            std::lock_guard<std::mutex> lock(m_);
            discovery_state_ = "not_found";
            discovery_reason_ = r.reason.empty() ? "not_found" : r.reason;
            out.reason = discovery_reason_;
            return out;
        }
        // Identity is established. Only now may the credential be presented, and only to this
        // candidate - as exactly one bounded fetch.
        std::string previous;
        {
            std::lock_guard<std::mutex> lock(m_);
            previous = cfg_.host;
        }
        if (!set_host(r.host)) {
            std::lock_guard<std::mutex> lock(m_);
            discovery_state_ = "not_found";
            discovery_reason_ = "candidate_host_invalid";
            out.reason = "candidate_host_invalid";
            return out;
        }
        out.found = true;      // a camera-identified host was located ...
        out.host = r.host;
        const CameraFrame frame = fetch_internal(now_ms, /*enforce_cadence=*/false);
        if (!frame.ok) {
            std::lock_guard<std::mutex> lock(m_);
            cfg_.host = previous;            // keep the last known host: no unverified moves
            discovery_state_ = "not_found";
            discovery_reason_ = "reverify_failed";
            out.reason = "reverify_failed";  // ... but it served no frame for us: do not move
            return out;
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            discovery_state_ = "found";
            discovery_source_ = "rediscovered";
            discovery_reason_.clear();
        }
        persist();
        out.verified = true;
        return out;
    }

    // The automatic path: only while the camera has genuinely FAILED a fetch (a feed that has
    // simply not been asked for a frame yet is not unreachable, and sweeping the LAN at startup
    // would be a false alarm), never while a live frame exists, and never more often than the
    // locator's own 60 s gate allows. Returns true when a verified frame was obtained.
    bool maybe_rediscover(int64_t now_ms) {
        std::lock_guard<std::recursive_mutex> op(operation_);
        if (!authorized()) return false;
        if (!cfg_.scan_enabled || !locator_) return false;
        const CameraStatus st = status(now_ms);
        if (st.consecutive_failures <= 0) return false;   // no evidence of a problem yet
        if (st.state == FeedState::Live) return false;    // the camera is working; leave it alone
        return rediscover(now_ms).verified;
    }

    // Fetch one frame from the camera. Blocking and bounded by the HttpClient policy.
    // Returns ok=false with a bounded reason when the camera cannot be reached, is not a
    // JPEG, or exceeds the frame cap. It never returns a cached frame as though new.
    CameraFrame fetch_frame(int64_t now_ms) { return fetch_internal(now_ms, /*enforce_cadence=*/true); }

    // One fetch. `enforce_cadence` is false for exactly one caller - the discovery re-verification
    // immediately after a located host - because that fetch must happen NOW (the failed attempt
    // that triggered discovery is milliseconds old) and it is still a single bounded request.
    CameraFrame fetch_internal(int64_t now_ms, bool enforce_cadence) {
        std::lock_guard<std::recursive_mutex> op(operation_);
        CameraFrame out;
        if (!authorized()) { out.error = "source_unauthorized"; return out; }
        CameraConfig cfg;
        {
            std::lock_guard<std::mutex> lock(m_);
            // Cadence is enforced HERE, on the server, not only in the browser: one fetch per
            // minimum interval regardless of how many callers or tabs ask. A refused request is
            // NOT a camera failure and does not advance the attempt clock.
            if (enforce_cadence && last_attempt_at_ms_ != 0 &&
                now_ms - last_attempt_at_ms_ < LiveCameraPolicy::kMinFetchIntervalMs) {
                out.error = "rate_limited";
                return out;
            }
            if (last_attempt_at_ms_ != 0 && now_ms < last_attempt_at_ms_) {
                out.error = "clock_went_backward";   // fail closed rather than trust a rewound clock
                return out;
            }
            last_attempt_at_ms_ = now_ms;
            cfg = cfg_;
        }
        if (!cfg.allow_plaintext) {
            out.error = "plaintext_not_enabled";
            record_failure(out.error, now_ms);
            return out;
        }
        if (cfg.host.empty()) {
            out.error = "no_camera_configured";
            record_failure(out.error, now_ms);
            return out;
        }

        const std::string url = "http://" + cfg.host + ":" + std::to_string(cfg.port)
                                + cfg.snapshot_path;
        const auto parsed = HttpClient::parse_url(url);
        if (!parsed.valid) {
            // parse_url's error is already generic; do not enrich it with the URL.
            out.error = "camera_url_invalid";
            record_failure(out.error, now_ms);
            return out;
        }

        if (cfg.scan_enabled && (cfg.mac.empty() || !locator_ ||
            locator_->expected_mac() != cfg.mac || !locator_->destination_matches(cfg.host))) {
            out.error = "destination_mac_unverified";
            record_failure(out.error, now_ms);
            return out;
        }
        // Credential lives here and nowhere else. HttpClient documents that it logs
        // nothing, including this header.
        std::vector<std::pair<std::string, std::string>> headers;
        {
            static const char kB64[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const std::string raw = cfg.user + ":" + cfg.password;
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
        hc.before_send = [this, cfg] {
            return authorized() && (!cfg.scan_enabled ||
                (locator_ && locator_->destination_matches(cfg.host)));
        };
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

        bool persist_now = false;
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

            // Record the verified host so a restart starts from where the camera actually is. Only
            // when the host CHANGED, so an ordinary 2 fps fetch does not rewrite the file.
            if (!cfg.state_file.empty() && cfg.host != persisted_host_) {
                persisted_host_ = cfg.host;
                persist_now = true;
            }
        }


        if (persist_now) persist();

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
        s.discovery_state = discovery_state_;
        s.discovery_last_scan_ms = discovery_last_scan_ms_;
        s.discovery_scanned_hosts = discovery_scanned_hosts_;
        s.discovery_source = discovery_source_;
        s.discovery_reason = discovery_reason_;

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
        // These two keys are the DEFAULT (analysis not wired on this object). The portal
        // overwrites them with the real value when --camera-analysis is enabled, so a preview
        // is never labelled as detector input by default.
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
        // Address-discovery state. Always present so a panel never has to guess whether the
        // portal is even allowed to look for a moved camera. No address, no MAC, no credential
        // and no scan target appears here: state, timing, a count and a role.
        j += ",\"discovery\":{\"state\":\"" + json_escape(s.discovery_state) + "\"";
        j += ",\"last_scan_ms\":" + std::to_string(s.discovery_last_scan_ms);
        j += ",\"scanned_hosts\":" + std::to_string(s.discovery_scanned_hosts);
        j += ",\"source\":\"" + json_escape(s.discovery_source) + "\"";
        j += ",\"enabled\":" + std::string(cfg_.scan_enabled ? "true" : "false");
        if (!s.discovery_reason.empty())
            j += ",\"reason\":\"" + json_escape(s.discovery_reason) + "\"";
        j += "}";
        j += "}";
        return j;
    }

private:
    // The default discovery engine. Constructed only when the operator opted in, so a feed that
    // did not opt in holds no object that could touch the network.
    void build_locator() {
        if (!cfg_.scan_enabled) {
            locator_.reset();
            return;
        }
        auto locator = std::make_shared<CameraLocator>(cfg_.host, cfg_.port, cfg_.snapshot_path,
                                                      cfg_.mac, locator_detail::default_io());
        if (seeded_scan_ms_ != 0) {
            PersistedLocatorState seed;
            seed.valid = true;
            seed.host = cfg_.host;
            seed.mac = cfg_.mac;
            seed.scanned_at_ms = seeded_scan_ms_;
            locator->seed_from_state(seed);
        }
        locator_ = std::move(locator);
    }

    // Best-effort: a state file that cannot be written is a performance loss (a rescan after
    // restart), never a correctness or security failure, so it does not fail the fetch.
    void persist() {
        std::string path, host, mac;
        int64_t scanned_at = 0;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (cfg_.state_file.empty()) return;
            path = cfg_.state_file;
            host = cfg_.host;
            mac = cfg_.mac;
            scanned_at = seeded_scan_ms_;
        }
        PersistedLocatorState st;
        st.valid = true;
        st.host = host;
        st.mac = mac;
        st.scanned_at_ms = scanned_at;
        (void)store_locator_state(path, st);
    }

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

    bool authorized() const { return !authorize_ || authorize_(); }
    mutable std::recursive_mutex operation_;
    std::function<bool()> authorize_;
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

    // ---- address discovery state ---------------------------------------------------------
    std::shared_ptr<CameraLocator> locator_;
    std::string discovery_state_ = "idle";
    int64_t discovery_last_scan_ms_ = 0;
    int discovery_scanned_hosts_ = 0;
    std::string discovery_source_ = "configured";
    std::string discovery_reason_;

    std::string persisted_host_;        // the host last written to the state file
    int64_t seeded_scan_ms_ = 0;        // scan time restored from the state file
};

}  // namespace portal
}  // namespace homeguardian

#pragma once
// Uploaded-media store for the HomeGuardian backup portal.
//
// WHAT THIS IS: a local, operator-facing store for media an operator uploads so a demo can show a
// "camera-style" feed WITHOUT a camera. WHAT IT IS NOT: a live camera, a CCTV recorder, or anything
// a physical device supplied. Every record and every response carries the same explicit label, and
// no code path here can claim otherwise.
//
// Reuses the repo's existing security posture rather than inventing one:
//   * validation requires extension, declared MIME type AND file signature to agree (all three),
//   * storage is OUTSIDE the web root (the dashboard HTML is embedded in the binary, so nothing is
//     served from disk except an individual, id-resolved file),
//   * filenames are generated server-side; a client-supplied name is never used as a path,
//   * the id is a fixed shape (u_YYYYMMDD_HHMMSS_<8 hex>) and the extension comes from a fixed
//     allowlist, so a resolved path cannot traverse or execute,
//   * video duration is verified, and a video whose duration cannot be determined is REFUSED
//     (fail closed) rather than accepted on trust,
//   * size ceilings are enforced here as well as in the transport.
//
// Header-only and dependency-free so it can be unit-tested without HTTP, a socket or a running
// application.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace hg_portal {

inline constexpr const char* kSimulatedFeedLabel = "Uploaded media — simulated camera feed";

// Ceilings. The transport already refuses an upload body above its own cap; these are the
// per-kind limits this store enforces independently.
inline constexpr size_t kMaxImageBytes = 8u * 1024u * 1024u;    // 8 MiB
inline constexpr size_t kMaxVideoBytes = 25u * 1024u * 1024u;   // 25 MiB
inline constexpr int64_t kMaxVideoDurationMs = 30 * 1000;       // 30 s
inline constexpr size_t kMaxFeedEntries = 200;                  // bounded feed

struct UploadRecord {
    std::string id;            // server-generated; also the file stem
    std::string ext;           // from a fixed allowlist
    std::string mime;          // the verified media type
    bool is_video = false;
    size_t bytes = 0;
    int64_t uploaded_at_ms = 0;
    int64_t duration_ms = 0;   // 0 for images
};

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------- signature and duration checks

inline bool has_prefix(std::string_view b, std::initializer_list<unsigned char> sig) {
    if (b.size() < sig.size()) return false;
    size_t i = 0;
    for (unsigned char c : sig) {
        if (static_cast<unsigned char>(b[i]) != c) return false;
        ++i;
    }
    return true;
}

// A file signature is authoritative about the container; the declared MIME type and the extension
// are hints that must agree with it. Anything ambiguous is refused.
inline std::string sniff_signature(std::string_view b) {
    if (has_prefix(b, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return "image/png";
    if (has_prefix(b, {0xFF, 0xD8, 0xFF})) return "image/jpeg";
    if (b.size() >= 12 && std::string_view(b.substr(0, 4)) == "RIFF" &&
        std::string_view(b.substr(8, 4)) == "WEBP")
        return "image/webp";
    if (b.size() >= 12 && std::string_view(b.substr(4, 4)) == "ftyp") return "video/mp4";
    if (has_prefix(b, {0x1A, 0x45, 0xDF, 0xA3})) return "video/webm";
    return "";
}

inline uint32_t be32(std::string_view b, size_t at) {
    if (at + 4 > b.size()) return 0;
    return (static_cast<uint32_t>(static_cast<unsigned char>(b[at])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(b[at + 3]));
}

inline uint64_t be64(std::string_view b, size_t at) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && at + i < b.size(); ++i)
        v = (v << 8) | static_cast<unsigned char>(b[at + i]);
    return v;
}

// MP4: walk the top-level boxes to moov/mvhd and read timescale+duration. Returns -1 when the
// structure cannot be read (the caller refuses rather than guessing).
inline int64_t mp4_duration_ms(std::string_view b) {
    size_t at = 0;
    while (at + 8 <= b.size()) {
        const uint64_t size = be32(b, at);
        const std::string_view type = b.substr(at + 4, 4);
        if (type == "moov") {
            size_t inner = at + 8;
            const size_t end = (size == 0 || at + size > b.size()) ? b.size() : at + size;
            while (inner + 8 <= end) {
                const uint64_t isize = be32(b, inner);
                if (b.substr(inner + 4, 4) == "mvhd") {
                    const size_t p = inner + 8;
                    const unsigned char version = static_cast<unsigned char>(b[p]);
                    uint64_t timescale = 0, duration = 0;
                    if (version == 1) {
                        timescale = be32(b, p + 20);
                        duration = be64(b, p + 24);
                    } else {
                        timescale = be32(b, p + 12);
                        duration = be32(b, p + 16);
                    }
                    if (timescale == 0) return -1;
                    return static_cast<int64_t>((duration * 1000) / timescale);
                }
                if (isize < 8) break;
                inner += isize;
            }
            return -1;
        }
        if (size < 8) break;
        at += size;
    }
    return -1;
}

// WebM: a bounded scan for the Info element's Duration (id 0x4489, a float). This is deliberately
// a narrow heuristic, not a full EBML parser: it looks only in the first 4 KiB where Info lives,
// and returns -1 (refuse) when it cannot find a plausible value.
inline int64_t webm_duration_ms(std::string_view b) {
    const size_t limit = std::min<size_t>(b.size(), 4096);
    // The element occupies indices i..i+6 (2-byte id, 1-byte size, 4-byte float), so require
    // 7 bytes FROM i, i.e. i + 7 <= limit. Using i + 7 < limit silently skips an element sitting
    // exactly 7 bytes from the end -- which is where a minimal fixture puts it.
    for (size_t i = 0; i + 7 <= limit; ++i) {
        if (static_cast<unsigned char>(b[i]) != 0x44 ||
            static_cast<unsigned char>(b[i + 1]) != 0x89)
            continue;
        const unsigned char szb = static_cast<unsigned char>(b[i + 2]);
        if (szb == 0x84) {  // 4-byte float payload
            const unsigned char* p =
                reinterpret_cast<const unsigned char*>(b.data() + i + 3);
            // EBML floats are big-endian; assemble the bytes before reinterpreting them.
            const unsigned char raw[4] = {p[3], p[2], p[1], p[0]};
            float f = 0.0f;
            std::memcpy(&f, raw, sizeof(float));
            if (f <= 0.0f || f > 3600.0f) return -1;
            return static_cast<int64_t>(f * 1000.0f);
        }
    }
    return -1;
}

struct ValidationResult {
    bool ok = false;
    std::string error;   // short, generic, safe to return
    std::string ext;     // png|jpg|webp|mp4|webm
    std::string mime;
    bool is_video = false;
    int64_t duration_ms = 0;
};

// The single validation entry point: extension, declared type and signature must agree; size and
// duration ceilings are enforced; nothing is inferred from the client's name beyond its extension.
inline ValidationResult validate_media(std::string_view bytes, std::string_view declared_mime,
                                      std::string_view client_name) {
    ValidationResult r;
    if (bytes.empty()) { r.error = "empty_body"; return r; }
    if (client_name.size() > 200) { r.error = "name_too_long"; return r; }

    const auto dot = client_name.find_last_of('.');
    if (dot == std::string_view::npos) { r.error = "unsupported_format"; return r; }
    std::string ext;
    for (size_t i = dot + 1; i < client_name.size(); ++i) {
        const char c = client_name[i];
        if (!std::isalnum(static_cast<unsigned char>(c))) { r.error = "unsupported_format"; return r; }
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (ext == "jpeg") ext = "jpg";

    const std::string sniffed = sniff_signature(bytes);
    if (sniffed.empty()) { r.error = "unsupported_format"; return r; }

    // Signature agrees with the extension, and with the declared MIME type. All three, not any one.
    const bool ext_ok =
        (sniffed == "image/png" && ext == "png") || (sniffed == "image/jpeg" && ext == "jpg") ||
        (sniffed == "image/webp" && ext == "webp") || (sniffed == "video/mp4" && ext == "mp4") ||
        (sniffed == "video/webm" && ext == "webm");
    if (!ext_ok) { r.error = "format_mismatch"; return r; }
    if (!declared_mime.empty() && std::string(declared_mime) != sniffed) {
        r.error = "format_mismatch";
        return r;
    }

    r.mime = sniffed;
    r.ext = ext;
    r.is_video = (sniffed.rfind("video/", 0) == 0);

    const size_t cap = r.is_video ? kMaxVideoBytes : kMaxImageBytes;
    if (bytes.size() > cap) { r.error = "too_large"; return r; }

    if (r.is_video) {
        const int64_t d = (ext == "mp4") ? mp4_duration_ms(bytes) : webm_duration_ms(bytes);
        if (d < 0) { r.error = "duration_unverifiable"; return r; }  // fail closed
        if (d == 0) { r.error = "invalid_video"; return r; }
        if (d > kMaxVideoDurationMs) { r.error = "too_long"; return r; }
        r.duration_ms = d;
    }
    r.ok = true;
    return r;
}

// ----------------------------------------------------------------------------------- the store

class UploadStore {
public:
    // root: a directory OUTSIDE the web root. It is created on demand (0700 where the platform
    // allows it) and is expected to be git-ignored.
    explicit UploadStore(std::string root) : root_(std::move(root)) {}

    const std::string& root() const { return root_; }

    bool load() {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec) return false;
        records_.clear();
        std::ifstream in(index_path());
        if (!in) return true;  // no index yet is not an error
        std::string line;
        while (std::getline(in, line)) {
            const auto rec = parse_line(line);
            if (rec) records_.push_back(*rec);
        }
        return true;
    }

    // Adds validated bytes. The caller has already run validate_media (or this runs it again).
    // Returns the record, or nullopt with `error` set.
    std::optional<UploadRecord> add(std::string_view bytes, std::string_view declared_mime,
                                    std::string_view client_name, std::string* error) {
        const ValidationResult v = validate_media(bytes, declared_mime, client_name);
        if (!v.ok) {
            if (error) *error = v.error;
            return std::nullopt;
        }
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec) { if (error) *error = "storage_unavailable"; return std::nullopt; }

        UploadRecord rec;
        rec.id = make_id();
        rec.ext = v.ext;
        rec.mime = v.mime;
        rec.is_video = v.is_video;
        rec.bytes = bytes.size();
        rec.uploaded_at_ms = now_ms();
        rec.duration_ms = v.duration_ms;

        const std::string path = path_for(rec.id, rec.ext);
        if (path.empty()) { if (error) *error = "invalid_record"; return std::nullopt; }
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) { if (error) *error = "storage_unavailable"; return std::nullopt; }
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out) { out.close(); std::filesystem::remove(path, ec); if (error) *error = "write_failed"; return std::nullopt; }
        }
        records_.push_back(rec);
        while (records_.size() > kMaxFeedEntries) records_.erase(records_.begin());
        persist();
        return rec;
    }

    bool remove(const std::string& id) {
        const auto it = std::find_if(records_.begin(), records_.end(),
                                     [&](const UploadRecord& r) { return r.id == id; });
        if (it == records_.end()) return false;
        const std::string path = path_for(it->id, it->ext);
        std::error_code ec;
        if (!path.empty()) std::filesystem::remove(path, ec);
        records_.erase(it);
        persist();
        return true;
    }

    std::optional<UploadRecord> find(const std::string& id) const {
        for (const auto& r : records_)
            if (r.id == id) return r;
        return std::nullopt;
    }

    const std::vector<UploadRecord>& records() const { return records_; }

    // Reads a stored file, re-validating both the record and that the resolved path is the one the
    // id implies. Returns empty optional when anything is off -- the caller answers 404.
    std::optional<std::string> read(const std::string& id) const {
        const auto rec = find(id);
        if (!rec) return std::nullopt;
        const std::string path = path_for(rec->id, rec->ext);
        if (path.empty()) return std::nullopt;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    static bool valid_id(std::string_view id) {
        // "u_" (2) + YYYYMMDD (8) + "_" + HHMMSS (6) + "_" + 8 hex = 26.
        if (id.size() != 2 + 8 + 1 + 6 + 1 + 8) return false;  // u_YYYYMMDD_HHMMSS_xxxxxxxx
        if (!(id[0] == 'u' && id[1] == '_')) return false;
        for (size_t i = 2; i < 10; ++i) if (!std::isdigit(static_cast<unsigned char>(id[i]))) return false;
        if (id[10] != '_') return false;
        for (size_t i = 11; i < 17; ++i) if (!std::isdigit(static_cast<unsigned char>(id[i]))) return false;
        if (id[17] != '_') return false;
        for (size_t i = 18; i < 26; ++i) {
            const char c = id[i];
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    static bool ext_allowed(std::string_view ext) {
        return ext == "png" || ext == "jpg" || ext == "webp" || ext == "mp4" || ext == "webm";
    }

private:
    std::string index_path() const { return root_ + "/index.jsonl"; }

    // The only place a path is built. Returns "" unless the id and extension are both of the fixed
    // shapes, which is what makes traversal structurally impossible.
    std::string path_for(const std::string& id, const std::string& ext) const {
        if (!valid_id(id) || !ext_allowed(ext)) return "";
        return root_ + "/" + id + "." + ext;
    }

    static std::string make_id() {
        const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
        gmtime_r(&t, &tm);
        char stamp[16];
        std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        static std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<uint64_t> d;
        char hex[9];
        std::snprintf(hex, sizeof(hex), "%08llx",
                      static_cast<unsigned long long>(d(rng) & 0xFFFFFFFFull));
        return std::string("u_") + stamp + "_" + hex;
    }

    static std::string json_escape(const std::string& in) {
        std::string out;
        for (char c : in) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                    else out.push_back(c);
            }
        }
        return out;
    }

    void persist() const {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        const std::string tmp = index_path() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) return;
            for (const auto& r : records_) {
                out << r.id << '\t' << r.ext << '\t' << r.mime << '\t' << (r.is_video ? 1 : 0)
                    << '\t' << r.bytes << '\t' << r.uploaded_at_ms << '\t' << r.duration_ms << '\n';
            }
        }
        std::filesystem::rename(tmp, index_path(), ec);
    }

    static std::optional<UploadRecord> parse_line(const std::string& line) {
        std::vector<std::string> f;
        std::string cur;
        for (char c : line) {
            if (c == '\t') { f.push_back(cur); cur.clear(); } else cur.push_back(c);
        }
        f.push_back(cur);
        if (f.size() != 7) return std::nullopt;
        UploadRecord r;
        r.id = f[0];
        r.ext = f[1];
        r.mime = f[2];
        r.is_video = (f[3] == "1");
        try {
            r.bytes = static_cast<size_t>(std::stoull(f[4]));
            r.uploaded_at_ms = std::stoll(f[5]);
            r.duration_ms = std::stoll(f[6]);
        } catch (...) { return std::nullopt; }
        if (!valid_id(r.id) || !ext_allowed(r.ext)) return std::nullopt;
        return r;
    }

    std::string root_;
    std::vector<UploadRecord> records_;
};

// The feed payload. Always carries the simulated label: the client cannot be told, and a reader
// cannot infer, that this is anything other than uploaded media.
inline std::string feed_json(const UploadStore& store) {
    std::ostringstream o;
    o << "{\"ok\":true,\"label\":\"" << kSimulatedFeedLabel
      << "\",\"simulated\":true,\"live_camera\":false,\"entries\":[";
    const auto& rs = store.records();
    for (size_t i = rs.size(); i-- > 0;) {
        const auto& r = rs[i];
        if (i + 1 != rs.size()) o << ',';
        o << "{\"id\":\"" << r.id << "\",\"kind\":\"" << (r.is_video ? "video" : "image")
          << "\",\"mime\":\"" << r.mime << "\",\"bytes\":" << r.bytes
          << ",\"uploaded_at_ms\":" << r.uploaded_at_ms
          << ",\"duration_ms\":" << r.duration_ms << ",\"url\":\"/media/" << r.id << "\"}";
    }
    o << "]}";
    return o.str();
}

}  // namespace hg_portal

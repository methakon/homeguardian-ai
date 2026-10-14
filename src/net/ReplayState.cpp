#include "net/ReplayState.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

namespace homeguardian::net {

namespace {

constexpr int kFormatVersion = 2;
constexpr size_t kMaxIdChars = 96;

std::string bounded(std::string text, size_t max_chars = 128) {
    if (text.size() > max_chars) text.resize(max_chars);
    for (char& c : text) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return text;
}

// The directory that holds the state file, so the RENAME itself can be made durable.
std::string parent_directory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string(".");
    if (slash == 0) return std::string("/");
    return path.substr(0, slash);
}

std::string errno_text() { return std::string(std::strerror(errno)); }

}  // namespace

ReplayState::Clock ReplayState::system_clock_ms() {
    return [] {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
    };
}

ReplayState::ReplayState(Config config, Clock clock)
    : config_(std::move(config)), clock_(std::move(clock)) {}

ReplayState::LoadOutcome ReplayState::load(std::string* reason) {
    auto fail = [reason](LoadOutcome outcome, const std::string& why) {
        if (reason != nullptr) *reason = why;
        return outcome;
    };
    if (!enabled()) {
        if (reason != nullptr) reason->clear();
        return LoadOutcome::absent;
    }

    std::ifstream in(config_.path, std::ios::binary);
    if (!in) {
        // No file yet is the normal first run.
        if (reason != nullptr) reason->clear();
        return LoadOutcome::absent;
    }
    nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
    if (parsed.is_discarded()) {
        return fail(LoadOutcome::malformed, "replay state is not valid JSON");
    }
    if (!parsed.is_object()) {
        return fail(LoadOutcome::malformed, "replay state is not an object");
    }
    if (!parsed.contains("devices") || !parsed["devices"].is_array()) {
        return fail(LoadOutcome::malformed, "replay state has no devices array");
    }
    if (parsed["devices"].size() > config_.max_devices) {
        return fail(LoadOutcome::unbounded, "replay state exceeds the device bound");
    }
    for (const auto& item : parsed["devices"]) {
        if (!item.is_object()) return fail(LoadOutcome::malformed, "entry is not an object");
        const std::string id = item.value("device_id", std::string());
        if (id.empty() || id.size() > kMaxIdChars) {
            return fail(LoadOutcome::malformed, "entry has no usable device_id");
        }
        if (!item.contains("last_sequence") || !item["last_sequence"].is_number_integer()) {
            return fail(LoadOutcome::malformed, std::string("entry has no usable watermark: ") + id);
        }
        Entry entry;
        entry.last_sequence = item["last_sequence"].get<uint64_t>();
        entry.last_observation_id = item.value("last_observation_id", std::string());
        auto existing = entries_.find(id);
        if (existing == entries_.end()) {
            entries_.emplace(id, std::move(entry));
        } else if (entry.last_sequence > existing->second.last_sequence) {
            existing->second = std::move(entry);  // duplicates: keep the highest, never the last
        }
    }
    for (const auto& entry : entries_) {
        watermarks_[entry.first] = entry.second.last_sequence;
    }
    if (reason != nullptr) reason->clear();
    return LoadOutcome::loaded;
}

ReplayState::Entry* ReplayState::find(const std::string& device_id) {
    auto it = entries_.find(device_id);
    return it == entries_.end() ? nullptr : &it->second;
}

bool ReplayState::reserve(const std::string& device_id, Entry** entry, std::string* reason) {
    Entry* found = find(device_id);
    if (found != nullptr) {
        *entry = found;
        return true;
    }
    if (entries_.size() >= config_.max_devices) {
        // Bounded: refuse rather than grow. The caller treats the refusal as a failed
        // acceptance, so a full store cannot silently drop protection.
        if (reason != nullptr) *reason = "replay state is at capacity";
        ++write_failures_;
        return false;
    }
    *entry = &entries_[device_id];
    watermarks_[device_id] = 0;  // keep the caller-visible mirror consistent from the start
    return true;
}

ReplayState::BeginOutcome ReplayState::begin(const std::string& device_id, uint64_t sequence,
                                             const std::string& observation_id,
                                             std::string* reason) {
    if (!enabled()) {
        if (reason != nullptr) reason->clear();
        return BeginOutcome::disabled;
    }
    if (reason != nullptr) reason->clear();

    Entry* entry = nullptr;
    if (!reserve(device_id, &entry, reason)) return BeginOutcome::unavailable;

    // Already completed at or above this sequence.
    if (sequence <= entry->last_sequence) {
        if (sequence == entry->last_sequence && entry->last_observation_id == observation_id) {
            // The uncertain-outcome window: the hub completed this observation but the
            // device never saw the acknowledgment. Answer it, do not process it twice.
            ++completed_repeats_;
            if (reason != nullptr) *reason = "already completed; answering idempotently";
            return BeginOutcome::completed_repeat;
        }
        if (reason != nullptr) *reason = "sequence is not newer than the completed watermark";
        return BeginOutcome::replayed;
    }

    const int64_t now_ms = clock_ ? clock_() : 0;
    if (entry->in_flight && entry->in_flight_sequence == sequence) {
        const bool fresh = entry->in_flight_marked_at_ms > 0 &&
                           now_ms - entry->in_flight_marked_at_ms < config_.in_flight_timeout_ms;
        if (fresh) {
            ++duplicate_refusals_;
            if (reason != nullptr) *reason = "the same sequence is already in flight";
            return BeginOutcome::duplicate;
        }
        // Stalled attempt: it never completed and never abandoned, so the retry proceeds.
    }
    // A different sequence being in flight is not a reason to refuse the newer one:
    // sequences may legitimately be skipped when an observation is dropped.
    entry->in_flight = true;
    entry->in_flight_sequence = sequence;
    entry->in_flight_marked_at_ms = now_ms;
    // NO durable write here. The marker guards this process; only completion is durable.
    return BeginOutcome::marked;
}

bool ReplayState::complete(const std::string& device_id, uint64_t sequence,
                           const std::string& observation_id, std::string* reason) {
    if (!enabled()) {
        if (reason != nullptr) reason->clear();
        return true;
    }
    Entry* entry = find(device_id);
    if (entry == nullptr) {
        if (reason != nullptr) *reason = "no replay state for this device";
        return false;
    }
    if (sequence <= entry->last_sequence) {
        // Idempotent: completed already. Keep the higher watermark.
        if (reason != nullptr) reason->clear();
        return true;
    }
    const Entry before = *entry;
    entry->last_sequence = sequence;
    entry->last_observation_id = observation_id;
    if (entry->in_flight && entry->in_flight_sequence == sequence) {
        entry->in_flight = false;
        entry->in_flight_sequence = 0;
        entry->in_flight_marked_at_ms = 0;
    }
    watermarks_[device_id] = sequence;
    if (!write_atomically(reason)) {
        *entry = before;
        watermarks_[device_id] = before.last_sequence;
        return false;
    }
    return true;
}

bool ReplayState::is_completed_repeat(const std::string& device_id, uint64_t sequence,
                                      const std::string& observation_id) const {
    if (!enabled() || observation_id.empty()) return false;
    const auto it = entries_.find(device_id);
    if (it == entries_.end()) return false;
    return it->second.last_sequence == sequence && it->second.last_observation_id == observation_id;
}

void ReplayState::abandon(const std::string& device_id, uint64_t sequence) {
    if (!enabled()) return;
    Entry* entry = find(device_id);
    if (entry == nullptr) return;
    if (!entry->in_flight || entry->in_flight_sequence != sequence) return;
    // In memory only: there is no durable marker to unwind, and if this clear is somehow
    // missed the timeout in begin() re-opens the sequence anyway.
    entry->in_flight = false;
    entry->in_flight_sequence = 0;
    entry->in_flight_marked_at_ms = 0;
}

bool ReplayState::flush(std::string* reason) {
    if (!enabled()) {
        if (reason != nullptr) reason->clear();
        return true;
    }
    return write_atomically(reason);
}

bool ReplayState::write_atomically(std::string* reason) {
    nlohmann::json document;
    document["version"] = kFormatVersion;
    document["devices"] = nlohmann::json::array();
    for (const auto& entry : entries_) {
        nlohmann::json item;
        item["device_id"] = entry.first;
        item["last_sequence"] = entry.second.last_sequence;
        item["last_observation_id"] = entry.second.last_observation_id;
        document["devices"].push_back(std::move(item));
    }

    // ATOMIC, DURABLE REPLACE. Each step is explicit and its failure is REPORTED:
    //   1. write a sibling temporary file,
    //   2. fsync it, so its bytes are on stable storage before it becomes THE file,
    //   3. rename(2) over the target - atomic within a directory, so a crash leaves
    //      either the old complete file or the new one, never a truncated mix,
    //   4. fsync the CONTAINING DIRECTORY, so the rename itself survives a power loss.
    // Step 4 is the one usually omitted: without it the file's contents can be durable
    // while the directory entry still points at the previous name, which on recovery looks
    // exactly like a lost watermark.
    const std::string temporary = config_.path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        if (reason != nullptr) *reason = "cannot open the replay state for writing: " + errno_text();
        ++write_failures_;
        return false;
    }
    const std::string text = document.dump();
    size_t written = 0;
    bool write_ok = true;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n <= 0) {
            if (errno == EINTR) continue;
            write_ok = false;
            break;
        }
        written += static_cast<size_t>(n);
    }
    if (write_ok && ::fsync(fd) != 0) {
        write_ok = false;
    }
    ::close(fd);
    if (!write_ok) {
        if (reason != nullptr) *reason = "cannot write and sync the replay state: " + errno_text();
        ::unlink(temporary.c_str());
        ++write_failures_;
        return false;
    }
    if (std::rename(temporary.c_str(), config_.path.c_str()) != 0) {
        if (reason != nullptr) *reason = "cannot replace the replay state: " + errno_text();
        ::unlink(temporary.c_str());
        ++write_failures_;
        return false;
    }
    const std::string directory = parent_directory(config_.path);
    const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        if (reason != nullptr) {
            *reason = "cannot open the replay state directory to sync it: " + errno_text();
        }
        ++write_failures_;
        return false;
    }
    const bool dir_synced = ::fsync(dir_fd) == 0;
    ::close(dir_fd);
    if (!dir_synced) {
        if (reason != nullptr) {
            *reason = "cannot sync the replay state directory: " + errno_text();
        }
        ++write_failures_;
        return false;
    }
    ++writes_;
    if (reason != nullptr) reason->clear();
    return true;
}

}  // namespace homeguardian::net

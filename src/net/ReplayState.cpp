#include "net/ReplayState.h"

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

namespace homeguardian::net {

namespace {

constexpr int kFormatVersion = 1;
constexpr size_t kMaxIdChars = 96;

std::string bounded(std::string text, size_t max_chars = 128) {
    if (text.size() > max_chars) text.resize(max_chars);
    for (char& c : text) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return text;
}

}  // namespace

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
    for (const auto& entry : parsed["devices"]) {
        if (!entry.is_object()) return fail(LoadOutcome::malformed, "entry is not an object");
        const std::string id = entry.value("device_id", std::string());
        if (id.empty() || id.size() > kMaxIdChars) {
            return fail(LoadOutcome::malformed, "entry has no usable device_id");
        }
        if (!entry.contains("last_sequence") || !entry["last_sequence"].is_number_integer()) {
            return fail(LoadOutcome::malformed, std::string("entry has no usable watermark: ") + id);
        }
        const uint64_t value = entry["last_sequence"].get<uint64_t>();
        auto it = watermarks_.find(id);
        if (it == watermarks_.end()) {
            watermarks_.emplace(id, value);
        } else if (value > it->second) {
            it->second = value;  // duplicates: keep the highest, never the last
        }
    }
    if (reason != nullptr) reason->clear();
    return LoadOutcome::loaded;
}

bool ReplayState::record(const std::string& device_id, uint64_t last_sequence, std::string* reason) {
    if (!enabled()) {
        if (reason != nullptr) reason->clear();
        return true;
    }
    auto it = watermarks_.find(device_id);
    if (it == watermarks_.end()) {
        if (watermarks_.size() >= config_.max_devices) {
            // Bounded: refuse rather than grow. The caller treats a failed record as a
            // failed acceptance, so a full store cannot silently drop protection.
            if (reason != nullptr) *reason = "replay state is at capacity";
            ++write_failures_;
            return false;
        }
        watermarks_.emplace(device_id, last_sequence);
    } else {
        if (last_sequence <= it->second) {
            // Never lower a watermark.
            if (reason != nullptr) reason->clear();
            return true;
        }
        it->second = last_sequence;
    }
    return write_atomically(reason);
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
    for (const auto& entry : watermarks_) {
        nlohmann::json item;
        item["device_id"] = entry.first;
        item["last_sequence"] = entry.second;
        document["devices"].push_back(std::move(item));
    }

    // ATOMIC REPLACE: write a sibling temporary file, fsync it, then rename over the
    // target. rename(2) within a directory is atomic, so a crash or a kill mid-write
    // leaves the previous complete file in place instead of a truncated one that would
    // silently reduce replay protection.
    const std::string temporary = config_.path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (reason != nullptr) *reason = "cannot open the replay state for writing";
            ++write_failures_;
            return false;
        }
        out << document.dump();
        out.flush();
        if (!out) {
            if (reason != nullptr) *reason = "cannot write the replay state";
            ++write_failures_;
            return false;
        }
    }
    if (std::rename(temporary.c_str(), config_.path.c_str()) != 0) {
        if (reason != nullptr) *reason = "cannot replace the replay state";
        ++write_failures_;
        return false;
    }
    ++writes_;
    if (reason != nullptr) reason->clear();
    return true;
}

}  // namespace homeguardian::net

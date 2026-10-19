#include "action/ActionAuditLog.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <vector>

namespace homeguardian::action {

namespace {

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t choose(uint32_t e, uint32_t f, uint32_t g) {
    return (e & f) ^ (~e & g);
}

inline uint32_t majority(uint32_t a, uint32_t b, uint32_t c) {
    return (a & b) ^ (a & c) ^ (b & c);
}

inline uint32_t sig0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sig1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t theta0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t theta1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

std::string compute_sha256_hex(const std::string& input) {
    uint32_t H[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    uint64_t total_bits = static_cast<uint64_t>(input.size()) * 8;
    std::vector<uint8_t> msg(input.begin(), input.end());
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((total_bits >> (i * 8)) & 0xff));
    }

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t W[64];
        for (int t = 0; t < 16; ++t) {
            W[t] = (static_cast<uint32_t>(msg[chunk + t * 4]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + t * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + t * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(msg[chunk + t * 4 + 3]));
        }
        for (int t = 16; t < 64; ++t) {
            W[t] = theta1(W[t - 2]) + W[t - 7] + theta0(W[t - 15]) + W[t - 16];
        }

        uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
        uint32_t e = H[4], f = H[5], g = H[6], h = H[7];

        for (int t = 0; t < 64; ++t) {
            uint32_t T1 = h + sig1(e) + choose(e, f, g) + K256[t] + W[t];
            uint32_t T2 = sig0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + T1;
            d = c;
            c = b;
            b = a;
            a = T1 + T2;
        }

        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }

    char buf[65];
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x%08x%08x%08x%08x",
                  H[0], H[1], H[2], H[3], H[4], H[5], H[6], H[7]);
    return std::string(buf);
}

} // namespace

nlohmann::json ActionAuditEntry::to_json() const {
    return {
        {"sequence_num", sequence_num},
        {"timestamp_ms", timestamp_ms},
        {"request_id", request_id},
        {"alert_id", alert_id},
        {"action_type", action_type},
        {"event_name", event_name},
        {"execution_state", execution_state},
        {"reason", reason},
        {"metadata", metadata},
        {"prev_hash", prev_hash},
        {"hash", hash}
    };
}

ActionAuditLog::ActionAuditLog() {
    // Initial genesis hash (empty SHA-256)
    last_hash_ = compute_sha256_hex("GENESIS-ACTION-AUDIT-LOG-1.0");
}

std::string ActionAuditLog::compute_hash(
    uint64_t seq, int64_t ts, const std::string& req_id,
    const std::string& evt, const std::string& state,
    const std::string& prev) const {
    std::ostringstream ss;
    ss << seq << "|" << ts << "|" << req_id << "|" << evt << "|" << state << "|" << prev;
    return compute_sha256_hex(ss.str());
}

ActionAuditEntry ActionAuditLog::record_event(
    const std::string& request_id,
    const std::string& alert_id,
    ActionType action_type,
    const std::string& event_name,
    ExecutionState state,
    const std::string& reason,
    const nlohmann::json& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);

    ActionAuditEntry entry;
    entry.sequence_num = next_seq_++;
    entry.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    entry.request_id = request_id;
    entry.alert_id = alert_id;
    entry.action_type = action_type_to_string(action_type);
    entry.event_name = event_name;
    entry.execution_state = execution_state_to_string(state);
    entry.reason = reason;
    entry.metadata = metadata;
    entry.prev_hash = last_hash_;
    entry.hash = compute_hash(entry.sequence_num, entry.timestamp_ms, entry.request_id,
                              entry.event_name, entry.execution_state, entry.prev_hash);

    last_hash_ = entry.hash;
    entries_.push_back(entry);
    return entry;
}

ActionAuditEntry ActionAuditLog::record_request(
    const ActionRequest& req, const std::string& event_name, const std::string& reason) {
    return record_event(
        req.request_id,
        req.alert_id,
        req.action_type,
        event_name,
        ExecutionState::PendingAuthorization,
        reason,
        {
            {"target", req.target},
            {"required_scope", scope_to_string(req.required_scope)},
            {"confidence", req.confidence},
            {"idempotency_key", req.idempotency_key}
        }
    );
}

ActionAuditEntry ActionAuditLog::record_result(
    const ActionResult& res, const std::string& event_name) {
    return record_event(
        res.request_id,
        "",
        res.action_type,
        event_name,
        res.state,
        res.failure_reason,
        {
            {"success", res.success},
            {"duration_ms", res.duration_ms},
            {"attempts", res.attempts},
            {"idempotency_key", res.idempotency_key}
        }
    );
}

ActionAuditEntry ActionAuditLog::record_security_event(
    const std::string& event_name,
    const std::string& request_id,
    const std::string& reason,
    const nlohmann::json& metadata) {
    return record_event(
        request_id,
        "",
        ActionType::DashboardNotification,
        event_name,
        ExecutionState::RefusedUnauthorized,
        reason,
        metadata
    );
}

size_t ActionAuditLog::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::vector<ActionAuditEntry> ActionAuditLog::get_all_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::vector<ActionAuditEntry> ActionAuditLog::get_entries_for_request(const std::string& request_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ActionAuditEntry> results;
    for (const auto& e : entries_) {
        if (e.request_id == request_id) {
            results.push_back(e);
        }
    }
    return results;
}

std::vector<ActionAuditEntry> ActionAuditLog::get_entries_for_alert(const std::string& alert_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ActionAuditEntry> results;
    for (const auto& e : entries_) {
        if (e.alert_id == alert_id) {
            results.push_back(e);
        }
    }
    return results;
}

std::vector<ActionAuditEntry> ActionAuditLog::get_entries_by_state(ExecutionState state) const {
    std::string state_str = execution_state_to_string(state);
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ActionAuditEntry> results;
    for (const auto& e : entries_) {
        if (e.execution_state == state_str) {
            results.push_back(e);
        }
    }
    return results;
}

nlohmann::json ActionAuditLog::to_json_export() const {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries_) {
        arr.push_back(e.to_json());
    }
    return arr;
}

std::string ActionAuditLog::to_csv_export() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream ss;
    ss << "sequence_num,timestamp_ms,request_id,alert_id,action_type,event_name,execution_state,reason,prev_hash,hash\n";
    for (const auto& e : entries_) {
        ss << e.sequence_num << ","
           << e.timestamp_ms << ",\""
           << e.request_id << "\",\""
           << e.alert_id << "\",\""
           << e.action_type << "\",\""
           << e.event_name << "\",\""
           << e.execution_state << "\",\""
           << e.reason << "\",\""
           << e.prev_hash << "\",\""
           << e.hash << "\"\n";
    }
    return ss.str();
}

bool ActionAuditLog::verify_chain_integrity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string expected_prev = compute_sha256_hex("GENESIS-ACTION-AUDIT-LOG-1.0");

    for (const auto& e : entries_) {
        if (e.prev_hash != expected_prev) {
            return false;
        }
        std::string computed = compute_hash(e.sequence_num, e.timestamp_ms, e.request_id,
                                            e.event_name, e.execution_state, e.prev_hash);
        if (computed != e.hash) {
            return false;
        }
        expected_prev = e.hash;
    }
    return true;
}

void ActionAuditLog::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    next_seq_ = 1;
    last_hash_ = compute_sha256_hex("GENESIS-ACTION-AUDIT-LOG-1.0");
}

} // namespace homeguardian::action

#pragma once

#include "action/ActionContract.h"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace homeguardian::action {

struct ActionAuditEntry {
    uint64_t sequence_num = 0;
    int64_t timestamp_ms = 0;
    std::string request_id;
    std::string alert_id;
    std::string action_type;
    std::string event_name;
    std::string execution_state;
    std::string reason;
    nlohmann::json metadata;
    std::string prev_hash;
    std::string hash;

    nlohmann::json to_json() const;
};

class ActionAuditLog {
public:
    ActionAuditLog();
    ~ActionAuditLog() = default;

    // Append an audit event into the ledger with SHA-256 hash chaining
    ActionAuditEntry record_event(
        const std::string& request_id,
        const std::string& alert_id,
        ActionType action_type,
        const std::string& event_name,
        ExecutionState state,
        const std::string& reason = "",
        const nlohmann::json& metadata = nlohmann::json::object()
    );

    // Record from request / result / security event
    ActionAuditEntry record_request(const ActionRequest& req, const std::string& event_name, const std::string& reason = "");
    ActionAuditEntry record_result(const ActionResult& res, const std::string& event_name);
    ActionAuditEntry record_security_event(const std::string& event_name, const std::string& request_id, const std::string& reason, const nlohmann::json& metadata = nlohmann::json::object());

    // Queries
    size_t size() const;
    std::vector<ActionAuditEntry> get_all_entries() const;
    std::vector<ActionAuditEntry> get_entries_for_request(const std::string& request_id) const;
    std::vector<ActionAuditEntry> get_entries_for_alert(const std::string& alert_id) const;
    std::vector<ActionAuditEntry> get_entries_by_state(ExecutionState state) const;

    // Export & Verification
    nlohmann::json to_json_export() const;
    std::string to_csv_export() const;
    bool verify_chain_integrity() const;

    // Clear log (test fixture usage only)
    void clear();

private:
    std::string compute_hash(uint64_t seq, int64_t ts, const std::string& req_id,
                             const std::string& evt, const std::string& state,
                             const std::string& prev) const;

    mutable std::mutex mutex_;
    uint64_t next_seq_ = 1;
    std::string last_hash_;
    std::vector<ActionAuditEntry> entries_;
};

} // namespace homeguardian::action

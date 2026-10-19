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

// Checkpoint representation for tamper-evident verification
struct AuditCheckpoint {
    std::string checkpoint_id;
    uint64_t sequence_num = 0;
    int64_t timestamp_ms = 0;
    std::string tip_hash;
    std::string anchor_id;
    std::string anchor_signature; // Cryptographic attestation from anchor

    nlohmann::json to_json() const;
    static AuditCheckpoint from_json(const nlohmann::json& j);
};

// Verification status against an anchored checkpoint
enum class CheckpointVerificationStatus {
    Valid,
    LogTruncated,                  // Local log has fewer entries than the checkpointed sequence
    LogModified,                   // An entry preceding or at the checkpoint has been mutated
    UnauthorizedForkOrReplacement  // Tip hash at checkpoint sequence does not match anchored tip hash
};

struct CheckpointVerificationResult {
    CheckpointVerificationStatus status = CheckpointVerificationStatus::Valid;
    bool valid = true;
    std::string details;
};

// Abstract external checkpoint anchor (witness service)
class IExternalCheckpointAnchor {
public:
    virtual ~IExternalCheckpointAnchor() = default;
    virtual std::string get_anchor_id() const = 0;
    virtual bool anchor_checkpoint(const AuditCheckpoint& checkpoint) = 0;
    virtual std::vector<AuditCheckpoint> get_anchored_checkpoints() const = 0;
    virtual std::optional<AuditCheckpoint> get_latest_checkpoint() const = 0;
};

// In-memory mock external anchor (safe for local-only operation, zero cloud calls)
class MockExternalCheckpointAnchor : public IExternalCheckpointAnchor {
public:
    explicit MockExternalCheckpointAnchor(std::string anchor_id = "mock-secure-anchor-01");
    ~MockExternalCheckpointAnchor() override = default;

    std::string get_anchor_id() const override { return anchor_id_; }
    bool anchor_checkpoint(const AuditCheckpoint& checkpoint) override;
    std::vector<AuditCheckpoint> get_anchored_checkpoints() const override;
    std::optional<AuditCheckpoint> get_latest_checkpoint() const override;
    void clear();

private:
    std::string anchor_id_;
    mutable std::mutex mutex_;
    std::vector<AuditCheckpoint> checkpoints_;
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

    // Checkpoint & External Anchor Hardening (Roadmap R2)
    AuditCheckpoint create_checkpoint(const std::string& anchor_id = "local-witness") const;
    bool anchor_to(IExternalCheckpointAnchor& anchor);
    CheckpointVerificationResult verify_against_checkpoint(const AuditCheckpoint& checkpoint) const;

    // Testing fault injection methods (to prove detection of tampering, truncation, and replacement)
    void tamper_with_entry_for_testing(size_t index, const std::string& new_reason);
    void truncate_for_testing(size_t count);
    void replace_chain_for_testing(const std::vector<ActionAuditEntry>& new_entries);

    // Clear log (test fixture usage only)
    void clear();

private:
    std::string compute_hash(uint64_t seq, int64_t ts, const std::string& req_id,
                             const std::string& evt, const std::string& state,
                             const std::string& reason, const std::string& prev) const;

    mutable std::mutex mutex_;
    uint64_t next_seq_ = 1;
    std::string last_hash_;
    std::vector<ActionAuditEntry> entries_;
};

} // namespace homeguardian::action

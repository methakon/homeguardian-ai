#include "PersistenceAudit.h"

#include <stdexcept>

namespace homeguardian {

PersistenceAudit::PersistenceAudit(const std::string& path)
    : process_start_(std::chrono::system_clock::now()) {
    db_ = std::make_shared<SQLiteDatabase>();
    db_->open(path);  // throws on failure -> caller runs degraded

    schema_ = std::make_unique<SchemaManager>(db_);
    schema_->initialize();  // creates/migrates schema (transactional)

    alert_repo_ = std::make_unique<AlertRepository>(db_);

    // Record how many alerts already exist: these are "historical" (written by
    // a previous process). Alerts written by THIS process after this point are
    // "current-process" and are distinguishable by detected_at >= process_start_.
    historical_count_ = alert_repo_->count();
}

PersistenceAudit::~PersistenceAudit() {
    close();
}

bool PersistenceAudit::record_alert(const Alert& alert) {
    std::lock_guard<std::mutex> lk(m_);
    if (closed_) {
        // Cannot persist after close: treat as a write failure (degraded), not
        // a silent success, and do not throw.
        write_failures_.fetch_add(1);
        healthy_.store(false);
        return false;
    }
    try {
        alert_repo_->save(alert);
        alerts_written_.fetch_add(1);
        return true;
    } catch (const std::exception&) {
        // Write failed: record it, mark degraded, do NOT throw or corrupt state.
        write_failures_.fetch_add(1);
        healthy_.store(false);
        return false;
    }
}

size_t PersistenceAudit::total_alert_count() {
    std::lock_guard<std::mutex> lk(m_);
    if (closed_) return 0;
    try {
        return alert_repo_->count();
    } catch (const std::exception&) {
        healthy_.store(false);
        return 0;
    }
}

size_t PersistenceAudit::prune_older_than(std::chrono::system_clock::time_point cutoff) {
    std::lock_guard<std::mutex> lk(m_);
    if (closed_) return 0;
    try {
        return alert_repo_->delete_older_than(cutoff);
    } catch (const std::exception&) {
        healthy_.store(false);
        return 0;
    }
}

size_t PersistenceAudit::prune_older_than_age(std::chrono::system_clock::duration max_age,
                                               std::chrono::system_clock::time_point now) {
    // max_age <= 0 means "keep forever": no pruning.
    if (max_age <= std::chrono::system_clock::duration::zero()) {
        return 0;
    }
    auto cutoff = now - max_age;
    return prune_older_than(cutoff);
}

size_t PersistenceAudit::maybe_prune_retention(std::chrono::system_clock::duration max_age,
                                               std::chrono::system_clock::duration interval,
                                               std::chrono::system_clock::time_point now) {
    // Bounded periodic retention: prune at most once per `interval`, never on
    // every call. Returns the number deleted (0 when not due, disabled, or on
    // failure). The last-prune timestamp is recorded so the scheduler does not
    // re-prune on every main-loop tick.
    if (max_age <= std::chrono::system_clock::duration::zero()) {
        return 0;  // retention disabled (keep forever)
    }
    std::lock_guard<std::mutex> lk(m_);
    if (closed_) {
        // Cannot prune after close: treat as a failure (degraded), not a silent
        // success, and do not throw.
        healthy_.store(false);
        return 0;
    }
    if (last_retention_prune_.has_value()) {
        auto since = now - *last_retention_prune_;
        if (since >= std::chrono::system_clock::duration::zero() && since < interval) {
            return 0;  // not due yet
        }
    }
    last_retention_prune_ = now;
    auto cutoff = now - max_age;
    try {
        return alert_repo_->delete_older_than(cutoff);
    } catch (const std::exception&) {
        healthy_.store(false);
        return 0;
    }
}

void PersistenceAudit::close() {
    std::lock_guard<std::mutex> lk(m_);
    if (closed_) return;
    closed_ = true;
    if (db_ && db_->is_open()) {
        db_->close();
    }
}

}  // namespace homeguardian

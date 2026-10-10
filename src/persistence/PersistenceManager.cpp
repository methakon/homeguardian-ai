#include "PersistenceManager.h"
#include "core/Logger.h"

namespace homeguardian {

PersistenceManager::PersistenceManager(const std::string& db_path,
                                        std::optional<std::chrono::hours> event_retention,
                                        std::optional<std::chrono::hours> alert_retention)
    : db_path_(db_path),
      event_retention_(event_retention),
      alert_retention_(alert_retention) {}

PersistenceManager::~PersistenceManager() {
    close();
}

void PersistenceManager::open() {
    db_ = std::make_shared<SQLiteDatabase>();
    db_->open(db_path_);

    schema_mgr_ = std::make_unique<SchemaManager>(db_);
    schema_mgr_->initialize();

    event_repo_ = std::make_unique<EventRepository>(db_);
    alert_repo_ = std::make_unique<AlertRepository>(db_);

    Logger::get()->info("PersistenceManager: opened database at {}", db_path_);
}

void PersistenceManager::close() {
    if (db_ && db_->is_open()) {
        db_->close();
        Logger::get()->info("PersistenceManager: closed database");
    }
    event_repo_.reset();
    alert_repo_.reset();
    schema_mgr_.reset();
    db_.reset();
}

bool PersistenceManager::is_open() const {
    return db_ && db_->is_open();
}

void PersistenceManager::save_event(const Event& event) {
    if (!event_repo_) throw DatabaseException("PersistenceManager not open");
    event_repo_->save(event);
}

void PersistenceManager::save_alert(const Alert& alert) {
    if (!alert_repo_) throw DatabaseException("PersistenceManager not open");
    alert_repo_->save(alert);
}

std::optional<Event> PersistenceManager::find_event(const std::string& event_id) {
    if (!event_repo_) throw DatabaseException("PersistenceManager not open");
    return event_repo_->find_by_id(event_id);
}

std::optional<Alert> PersistenceManager::find_alert(const std::string& alert_id) {
    if (!alert_repo_) throw DatabaseException("PersistenceManager not open");
    return alert_repo_->find_by_id(alert_id);
}

std::vector<Event> PersistenceManager::get_recent_events(size_t limit) {
    if (!event_repo_) throw DatabaseException("PersistenceManager not open");
    return event_repo_->find_recent(limit);
}

std::vector<Alert> PersistenceManager::get_recent_alerts(size_t limit) {
    if (!alert_repo_) throw DatabaseException("PersistenceManager not open");
    return alert_repo_->find_recent(limit);
}

size_t PersistenceManager::event_count() {
    if (!event_repo_) throw DatabaseException("PersistenceManager not open");
    return event_repo_->count();
}

size_t PersistenceManager::alert_count() {
    if (!alert_repo_) throw DatabaseException("PersistenceManager not open");
    return alert_repo_->count();
}

size_t PersistenceManager::enforce_retention() {
    if (!event_repo_ || !alert_repo_) throw DatabaseException("PersistenceManager not open");

    size_t deleted = 0;
    auto now = std::chrono::system_clock::now();

    if (event_retention_.has_value()) {
        auto cutoff = now - event_retention_.value();
        deleted += event_repo_->delete_older_than(cutoff);
    }

    if (alert_retention_.has_value()) {
        auto cutoff = now - alert_retention_.value();
        deleted += alert_repo_->delete_older_than(cutoff);
    }

    if (deleted > 0) {
        Logger::get()->info("PersistenceManager: retention deleted {} records", deleted);
    }

    return deleted;
}

IEventRepository& PersistenceManager::event_repo() {
    if (!event_repo_) throw DatabaseException("PersistenceManager not open");
    return *event_repo_;
}

IAlertRepository& PersistenceManager::alert_repo() {
    if (!alert_repo_) throw DatabaseException("PersistenceManager not open");
    return *alert_repo_;
}

} // namespace homeguardian

#pragma once

#include "EventRepository.h"
#include "AlertRepository.h"
#include "SchemaManager.h"
#include "core/Event.h"
#include "core/Alert.h"
#include <memory>
#include <chrono>

namespace homeguardian {

class PersistenceManager {
public:
    PersistenceManager(const std::string& db_path,
                       std::optional<std::chrono::hours> event_retention = std::chrono::hours(24 * 30),
                       std::optional<std::chrono::hours> alert_retention = std::chrono::hours(24 * 90));
    ~PersistenceManager();

    PersistenceManager(const PersistenceManager&) = delete;
    PersistenceManager& operator=(const PersistenceManager&) = delete;

    void open();
    void close();
    bool is_open() const;

    void save_event(const Event& event);
    void save_alert(const Alert& alert);

    std::optional<Event> find_event(const std::string& event_id);
    std::optional<Alert> find_alert(const std::string& alert_id);

    std::vector<Event> get_recent_events(size_t limit);
    std::vector<Alert> get_recent_alerts(size_t limit);

    size_t event_count();
    size_t alert_count();

    size_t enforce_retention();

    IEventRepository& event_repo();
    IAlertRepository& alert_repo();

private:
    std::string db_path_;
    std::optional<std::chrono::hours> event_retention_;
    std::optional<std::chrono::hours> alert_retention_;

    std::shared_ptr<SQLiteDatabase> db_;
    std::unique_ptr<EventRepository> event_repo_;
    std::unique_ptr<AlertRepository> alert_repo_;
    std::unique_ptr<SchemaManager> schema_mgr_;
};

} // namespace homeguardian

#pragma once

#include "IAlertRepository.h"
#include "SQLiteDatabase.h"
#include <sqlite3.h>
#include <memory>

namespace homeguardian {

class AlertRepository : public IAlertRepository {
public:
    explicit AlertRepository(std::shared_ptr<SQLiteDatabase> db);
    ~AlertRepository() override = default;

    void save(const Alert& alert) override;
    std::optional<Alert> find_by_id(const std::string& alert_id) override;
    std::vector<Alert> find_recent(size_t limit) override;
    std::vector<Alert> find_by_severity(Severity severity) override;
    size_t count() override;
    size_t delete_older_than(std::chrono::system_clock::time_point cutoff) override;
    size_t delete_all() override;

private:
    std::shared_ptr<SQLiteDatabase> db_;

    Alert row_to_alert(sqlite3_stmt* stmt);
    void ensure_table();
};

} // namespace homeguardian

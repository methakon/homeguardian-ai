#pragma once

#include "IEventRepository.h"
#include "SQLiteDatabase.h"
#include <sqlite3.h>
#include <memory>

namespace homeguardian {

class EventRepository : public IEventRepository {
public:
    explicit EventRepository(std::shared_ptr<SQLiteDatabase> db);
    ~EventRepository() override = default;

    void save(const Event& event) override;
    std::optional<Event> find_by_id(const std::string& event_id) override;
    std::vector<Event> find_recent(size_t limit) override;
    std::vector<Event> find_by_correlation_id(const std::string& correlation_id) override;
    size_t count() override;
    size_t delete_older_than(std::chrono::system_clock::time_point cutoff) override;
    size_t delete_all() override;

private:
    std::shared_ptr<SQLiteDatabase> db_;

    Event row_to_event(sqlite3_stmt* stmt);
    void ensure_table();
};

} // namespace homeguardian

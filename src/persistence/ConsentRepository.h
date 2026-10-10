#pragma once

#include "IConsentRepository.h"
#include "SQLiteDatabase.h"
#include <sqlite3.h>
#include <memory>

namespace homeguardian {

class ConsentRepository : public IConsentRepository {
public:
    explicit ConsentRepository(std::shared_ptr<SQLiteDatabase> db);
    ~ConsentRepository() override = default;

    void save(const ConsentRecord& record) override;
    std::optional<ConsentRecord> find_by_id(const std::string& consent_id) override;
    std::vector<ConsentRecord> find_by_subject(const std::string& subject_scope) override;
    std::vector<ConsentRecord> find_by_purpose(const std::string& subject_scope, const std::string& purpose) override;
    size_t count() override;
    size_t delete_all() override;

private:
    std::shared_ptr<SQLiteDatabase> db_;

    ConsentRecord row_to_consent(sqlite3_stmt* stmt);
    void ensure_table();
};

} // namespace homeguardian

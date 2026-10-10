#pragma once

#include "IDatabase.h"
#include <sqlite3.h>
#include <memory>
#include <mutex>

namespace homeguardian {

class SQLiteDatabase : public IDatabase {
public:
    SQLiteDatabase();
    ~SQLiteDatabase() override;

    SQLiteDatabase(const SQLiteDatabase&) = delete;
    SQLiteDatabase& operator=(const SQLiteDatabase&) = delete;

    void open(const std::string& path) override;
    void close() override;
    bool is_open() const override;

    void begin_transaction() override;
    void commit() override;
    void rollback() override;

    int64_t get_user_version() override;
    void set_user_version(int64_t version) override;

    void execute(const std::string& sql) override;
    int64_t last_insert_rowid() override;
    int64_t rows_modified() override;

    sqlite3* raw() { return db_; }

private:
    void execute_locked(const std::string& sql);

    sqlite3* db_;
    bool in_transaction_;
    mutable std::mutex mutex_;
};

} // namespace homeguardian

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
    // Read-only view of an EXISTING database. It never creates the file or its parent directory, and the
    // connection cannot write: a missing or unusable store THROWS, so a reader fails closed instead of
    // opening an empty database and reporting "no data" for "no storage". That distinction is the whole
    // point - a reader that creates what it reads can silently mask a missing store as an empty one.
    void open_read_only(const std::string& path);
    // True when this connection is guaranteed unable to write (open_read_only was used).
    bool read_only_connection() const { return read_only_connection_; }
    // True when SQLITE_OPEN_READONLY alone was sufficient. False means the connection had to be opened
    // read-write for a WAL/-shm reason and then pinned with PRAGMA query_only, which is recorded rather
    // than hidden, because "verified WAL behaviour" should be an observation, not an assumption.
    bool opened_with_readonly_flag() const { return opened_with_readonly_flag_; }
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
    bool read_only_connection_ = false;
    bool opened_with_readonly_flag_ = false;
    mutable std::mutex mutex_;
};

} // namespace homeguardian

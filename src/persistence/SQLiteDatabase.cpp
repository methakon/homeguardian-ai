#include "SQLiteDatabase.h"
#include <filesystem>

namespace homeguardian {

SQLiteDatabase::SQLiteDatabase() : db_(nullptr), in_transaction_(false) {}

SQLiteDatabase::~SQLiteDatabase() {
    close();
}

void SQLiteDatabase::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        throw DatabaseException("Database already open");
    }

    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "unknown error";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw DatabaseException("Cannot open database: " + err);
    }

    sqlite3_busy_timeout(db_, 5000);
    execute_locked("PRAGMA foreign_keys = ON;");
    execute_locked("PRAGMA journal_mode = WAL;");
    // Enable extended result codes so callers can distinguish constraint
    // subtypes (e.g. SQLITE_CONSTRAINT_PRIMARYKEY vs SQLITE_CONSTRAINT_FOREIGNKEY).
    sqlite3_extended_result_codes(db_, 1);
}

void SQLiteDatabase::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    in_transaction_ = false;
}

bool SQLiteDatabase::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ != nullptr;
}

void SQLiteDatabase::begin_transaction() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) throw DatabaseException("Database not open");
    if (in_transaction_) throw DatabaseException("Transaction already in progress");
    char* err = nullptr;
    if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DatabaseException("BEGIN failed: " + msg);
    }
    in_transaction_ = true;
}

void SQLiteDatabase::commit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) throw DatabaseException("Database not open");
    if (!in_transaction_) throw DatabaseException("No transaction in progress");
    char* err = nullptr;
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        in_transaction_ = false;
        throw DatabaseException("COMMIT failed: " + msg);
    }
    in_transaction_ = false;
}

void SQLiteDatabase::rollback() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) throw DatabaseException("Database not open");
    if (!in_transaction_) throw DatabaseException("No transaction in progress");
    char* err = nullptr;
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err);
    sqlite3_free(err);
    in_transaction_ = false;
}

int64_t SQLiteDatabase::get_user_version() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) throw DatabaseException("Database not open");
    sqlite3_stmt* stmt = nullptr;
    int64_t version = 0;
    if (sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return version;
}

void SQLiteDatabase::set_user_version(int64_t version) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) throw DatabaseException("Database not open");
    std::string sql = "PRAGMA user_version = " + std::to_string(version) + ";";
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DatabaseException("set_user_version failed: " + msg);
    }
}

void SQLiteDatabase::execute(const std::string& sql) {
    std::lock_guard<std::mutex> lock(mutex_);
    execute_locked(sql);
}

void SQLiteDatabase::execute_locked(const std::string& sql) {
    if (!db_) throw DatabaseException("Database not open");
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DatabaseException("SQL execute failed: " + msg + " | SQL: " + sql);
    }
}

int64_t SQLiteDatabase::last_insert_rowid() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) throw DatabaseException("Database not open");
    return sqlite3_last_insert_rowid(db_);
}

int64_t SQLiteDatabase::rows_modified() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) throw DatabaseException("Database not open");
    return sqlite3_changes(db_);
}

} // namespace homeguardian

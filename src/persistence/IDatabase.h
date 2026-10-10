#pragma once

#include <string>
#include <stdexcept>

namespace homeguardian {

class DatabaseException : public std::runtime_error {
public:
    explicit DatabaseException(const std::string& msg) : std::runtime_error(msg) {}
};

class IDatabase {
public:
    virtual ~IDatabase() = default;

    virtual void open(const std::string& path) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual void begin_transaction() = 0;
    virtual void commit() = 0;
    virtual void rollback() = 0;

    virtual int64_t get_user_version() = 0;
    virtual void set_user_version(int64_t version) = 0;

    virtual void execute(const std::string& sql) = 0;
    virtual int64_t last_insert_rowid() = 0;
    virtual int64_t rows_modified() = 0;
};

} // namespace homeguardian

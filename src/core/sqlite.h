// Minimal RAII wrapper around SQLite3 for the Smart selling journals.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

struct sqlite3;
struct sqlite3_stmt;

namespace ng {

class SqliteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Sqlite;

class Statement {
public:
    Statement(Sqlite& db, std::string_view sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, std::string_view value);
    Statement& bind(int index, const std::string& value) { return bind(index, std::string_view(value)); }
    Statement& bind(int index, const char* value) { return bind(index, std::string_view(value)); }
    Statement& bind(int index, long long value);
    Statement& bind(int index, int value) { return bind(index, static_cast<long long>(value)); }
    Statement& bind(int index, double value);
    Statement& bind_null(int index);
    Statement& bind_optional(int index, const std::optional<std::string>& value);

    // Returns true while a row is available.
    bool step();
    // Executes to completion (for INSERT/UPDATE/DELETE).
    void exec();
    void reset();

    bool is_null(int column) const;
    std::string column_text(int column) const;
    long long column_int(int column) const;
    double column_double(int column) const;
    std::optional<std::string> column_optional_text(int column) const;
    int column_count() const;
    std::string column_name(int column) const;
    // The current row as a JSON object keyed by column name (INTEGER, REAL, TEXT and NULL map to
    // their JSON counterparts), like Python's dict(sqlite3.Row).
    json row_json() const;

private:
    Sqlite& db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class Sqlite {
public:
    enum class Mode {
        ReadWriteCreate,  // default: create the file when missing
        ReadWrite,        // the file must exist
        ReadOnly,         // the file must exist; no write is possible through this connection
    };
    explicit Sqlite(const std::string& path, Mode mode = Mode::ReadWriteCreate, int busy_timeout_ms = 5000);
    ~Sqlite();
    Sqlite(const Sqlite&) = delete;
    Sqlite& operator=(const Sqlite&) = delete;

    void exec(std::string_view sql);
    void begin_immediate();
    // Plain deferred BEGIN (a consistent read snapshot in WAL mode once the first read runs).
    void begin();
    // Online copy of this database into `target` (sqlite3_backup), like Python's Connection.backup().
    void backup_to(Sqlite& target);
    void commit();
    void rollback();
    bool in_transaction() const { return in_transaction_; }
    long long last_insert_rowid() const;
    int changes() const;
    void close();
    sqlite3* handle() const { return db_; }
    std::string error_message() const;

private:
    sqlite3* db_ = nullptr;
    bool in_transaction_ = false;
};

}  // namespace ng

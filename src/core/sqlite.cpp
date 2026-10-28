#include "core/sqlite.h"

#include <sqlite3.h>

namespace ng {

Sqlite::Sqlite(const std::string& path) {
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (path == ":memory:") flags |= SQLITE_OPEN_MEMORY;
    int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db_ ? sqlite3_errmsg(db_) : "unable to open database";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw SqliteError("sqlite open failed for " + path + ": " + message);
    }
    sqlite3_busy_timeout(db_, 5000);
}

Sqlite::~Sqlite() { close(); }

void Sqlite::close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

std::string Sqlite::error_message() const { return db_ ? sqlite3_errmsg(db_) : "database closed"; }

void Sqlite::exec(std::string_view sql) {
    if (!db_) throw SqliteError("database closed");
    char* err = nullptr;
    std::string statement(sql);
    int rc = sqlite3_exec(db_, statement.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string message = err ? err : error_message();
        sqlite3_free(err);
        throw SqliteError("sqlite exec failed: " + message + " [" + statement.substr(0, 120) + "]");
    }
}

void Sqlite::begin_immediate() {
    exec("BEGIN IMMEDIATE");
    in_transaction_ = true;
}

void Sqlite::commit() {
    in_transaction_ = false;
    exec("COMMIT");
}

void Sqlite::rollback() {
    in_transaction_ = false;
    try {
        exec("ROLLBACK");
    } catch (const SqliteError&) {
        // Rolling back a failed transaction can itself fail; the connection is left usable.
    }
}

long long Sqlite::last_insert_rowid() const { return db_ ? sqlite3_last_insert_rowid(db_) : 0; }

int Sqlite::changes() const { return db_ ? sqlite3_changes(db_) : 0; }

Statement::Statement(Sqlite& db, std::string_view sql) : db_(db) {
    if (!db.handle()) throw SqliteError("database closed");
    int rc = sqlite3_prepare_v2(db.handle(), sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) throw SqliteError("sqlite prepare failed: " + db.error_message() + " [" + std::string(sql) + "]");
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Statement& Statement::bind(int index, std::string_view value) {
    if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        throw SqliteError("sqlite bind failed: " + db_.error_message());
    }
    return *this;
}

Statement& Statement::bind(int index, long long value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) throw SqliteError("sqlite bind failed: " + db_.error_message());
    return *this;
}

Statement& Statement::bind(int index, double value) {
    if (sqlite3_bind_double(stmt_, index, value) != SQLITE_OK) throw SqliteError("sqlite bind failed: " + db_.error_message());
    return *this;
}

Statement& Statement::bind_null(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) throw SqliteError("sqlite bind failed: " + db_.error_message());
    return *this;
}

Statement& Statement::bind_optional(int index, const std::optional<std::string>& value) {
    return value ? bind(index, *value) : bind_null(index);
}

bool Statement::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw SqliteError("sqlite step failed: " + db_.error_message());
}

void Statement::exec() {
    while (step()) {
    }
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

bool Statement::is_null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

std::string Statement::column_text(int column) const {
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    if (!text) return "";
    int size = sqlite3_column_bytes(stmt_, column);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size));
}

std::optional<std::string> Statement::column_optional_text(int column) const {
    if (is_null(column)) return std::nullopt;
    return column_text(column);
}

long long Statement::column_int(int column) const { return sqlite3_column_int64(stmt_, column); }

double Statement::column_double(int column) const { return sqlite3_column_double(stmt_, column); }

int Statement::column_count() const { return sqlite3_column_count(stmt_); }

}  // namespace ng

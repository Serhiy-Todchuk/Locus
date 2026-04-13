#pragma once

#include <filesystem>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace locus {

namespace fs = std::filesystem;

// Thin RAII wrapper around SQLite. Owns the connection and creates
// the schema on first open. WAL mode is always enabled.
class Database {
public:
    explicit Database(const fs::path& db_path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* handle() { return db_; }

    // Execute a SQL statement that returns no rows.
    void exec(const char* sql);

    // Prepare a statement. Caller owns the returned handle.
    sqlite3_stmt* prepare(const char* sql);

private:
    void create_schema();

    sqlite3* db_ = nullptr;
};

} // namespace locus

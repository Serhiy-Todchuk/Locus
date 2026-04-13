#include "database.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace locus {

Database::Database(const fs::path& db_path)
{
    int rc = sqlite3_open(db_path.string().c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Cannot open database: " + err);
    }

    // WAL mode for concurrent reads + single writer
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");

    spdlog::trace("SQLite opened: {}", db_path.string());

    create_schema();
}

Database::~Database()
{
    if (db_) {
        sqlite3_close(db_);
        spdlog::trace("SQLite closed");
    }
}

void Database::exec(const char* sql)
{
    spdlog::trace("SQL exec: {}", sql);

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::string err = err_msg ? err_msg : "unknown error";
        sqlite3_free(err_msg);
        throw std::runtime_error("SQL error: " + err + " | query: " + sql);
    }
}

void Database::create_schema()
{
    // S0.2: files table only. FTS5, symbols, headings added in S0.3.
    exec(R"(
        CREATE TABLE IF NOT EXISTS files (
            id          INTEGER PRIMARY KEY,
            path        TEXT UNIQUE NOT NULL,
            abs_path    TEXT NOT NULL,
            size_bytes  INTEGER,
            modified_at INTEGER,
            ext         TEXT,
            is_binary   INTEGER DEFAULT 0,
            language    TEXT,
            indexed_at  INTEGER
        )
    )");

    spdlog::trace("Database schema initialised");
}

} // namespace locus

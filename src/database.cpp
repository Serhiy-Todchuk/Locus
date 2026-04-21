#include "database.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

// sqlite-vec: we compile the amalgamation with SQLITE_CORE, so when calling
// from application code we only need the init function prototype.
extern "C" {
    int sqlite3_vec_init(sqlite3* db, char** pzErrMsg,
                         const sqlite3_api_routines* pApi);
}
#define SQLITE_VEC_VERSION "v0.1.9"

#include <stdexcept>
#include <string>

namespace locus {

Database::Database(const fs::path& db_path, DbKind kind)
    : kind_(kind)
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

    if (kind_ == DbKind::Main) {
        create_main_schema();
    } else {
        load_sqlite_vec();
        create_vectors_schema();
    }
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

sqlite3_stmt* Database::prepare(const char* sql)
{
    spdlog::trace("SQL prepare: {}", sql);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        throw std::runtime_error("SQL prepare error: " + err + " | query: " + sql);
    }
    return stmt;
}

void Database::load_sqlite_vec()
{
    char* err_msg = nullptr;
    int rc = sqlite3_vec_init(db_, &err_msg, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = err_msg ? err_msg : "unknown error";
        sqlite3_free(err_msg);
        spdlog::warn("sqlite-vec init failed: {}", err);
    } else {
        spdlog::trace("sqlite-vec {} loaded", SQLITE_VEC_VERSION);
    }
}

void Database::create_main_schema()
{
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

    // FTS5 full-text search over file contents
    exec(R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5(
            path UNINDEXED,
            content,
            tokenize='porter unicode61'
        )
    )");

    // Code symbols extracted by Tree-sitter
    exec(R"(
        CREATE TABLE IF NOT EXISTS symbols (
            id          INTEGER PRIMARY KEY,
            file_id     INTEGER REFERENCES files(id),
            kind        TEXT,
            name        TEXT NOT NULL,
            line_start  INTEGER,
            line_end    INTEGER,
            signature   TEXT,
            parent_name TEXT
        )
    )");
    exec("CREATE INDEX IF NOT EXISTS symbols_name ON symbols(name)");
    exec("CREATE INDEX IF NOT EXISTS symbols_file ON symbols(file_id)");

    // Document headings / outline
    exec(R"(
        CREATE TABLE IF NOT EXISTS headings (
            id          INTEGER PRIMARY KEY,
            file_id     INTEGER REFERENCES files(id),
            level       INTEGER,
            text        TEXT,
            line_number INTEGER
        )
    )");
    exec("CREATE INDEX IF NOT EXISTS headings_file ON headings(file_id)");

    spdlog::trace("Main DB schema initialised");
}

void Database::create_vectors_schema()
{
    // Semantic chunks.  `file_id` is a plain integer — the referenced `files`
    // row lives in a different database file so no FK constraint.
    exec(R"(
        CREATE TABLE IF NOT EXISTS chunks (
            id          INTEGER PRIMARY KEY,
            file_id     INTEGER NOT NULL,
            chunk_index INTEGER,
            start_line  INTEGER,
            end_line    INTEGER,
            content     TEXT NOT NULL
        )
    )");
    exec("CREATE INDEX IF NOT EXISTS chunks_file ON chunks(file_id)");

    // Vector index for semantic similarity (sqlite-vec)
    exec(R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS chunk_vectors USING vec0(
            chunk_id INTEGER PRIMARY KEY,
            embedding float[384]
        )
    )");

    spdlog::trace("Vectors DB schema initialised");
}

void Database::drop_legacy_semantic_tables()
{
    // Best-effort cleanup of the pre-split layout: if a Main DB still carries
    // `chunks` / `chunk_vectors`, drop them so schema stays in sync with the
    // new design.  vec0 virtual tables need the extension loaded to drop
    // cleanly; if it's not loaded here, the DROP on chunk_vectors will fail
    // silently and leave the shadow tables — acceptable because main DB never
    // reads them anyway.
    sqlite3_stmt* check = nullptr;
    const char* sql =
        "SELECT name FROM sqlite_master "
        "WHERE type IN ('table','view') AND name IN ('chunks','chunk_vectors')";
    if (sqlite3_prepare_v2(db_, sql, -1, &check, nullptr) != SQLITE_OK) return;

    bool had_any = false;
    while (sqlite3_step(check) == SQLITE_ROW) { had_any = true; break; }
    sqlite3_finalize(check);

    if (!had_any) return;

    spdlog::info("Dropping legacy semantic tables from main index.db");

    // Try to load sqlite-vec so DROP on chunk_vectors succeeds.
    load_sqlite_vec();

    char* err_msg = nullptr;
    sqlite3_exec(db_, "DROP TABLE IF EXISTS chunk_vectors", nullptr, nullptr, &err_msg);
    if (err_msg) { sqlite3_free(err_msg); err_msg = nullptr; }
    sqlite3_exec(db_, "DROP TABLE IF EXISTS chunks", nullptr, nullptr, &err_msg);
    if (err_msg) sqlite3_free(err_msg);
}

} // namespace locus

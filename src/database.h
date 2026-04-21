#pragma once

#include <filesystem>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace locus {

namespace fs = std::filesystem;

// Which on-disk schema this connection owns.  The `Main` kind holds the
// deterministic skeleton (files, FTS5, symbols, headings).  The `Vectors` kind
// holds the semantic layer (chunks + vec0 chunk_vectors) and loads the
// sqlite-vec extension.  They live in separate files so the embedding worker
// and the indexer write through independent WALs.
enum class DbKind {
    Main,
    Vectors,
};

// Thin RAII wrapper around SQLite. Owns the connection and creates the
// appropriate schema on open. WAL mode is always enabled.
class Database {
public:
    Database(const fs::path& db_path, DbKind kind);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* handle() { return db_; }
    DbKind kind() const { return kind_; }

    // Execute a SQL statement that returns no rows.
    void exec(const char* sql);

    // Prepare a statement. Caller owns the returned handle.
    sqlite3_stmt* prepare(const char* sql);

    // Idempotent: drop `chunks` / `chunk_vectors` if they were left behind in a
    // legacy Main DB from before the split.  Only meaningful on Main.
    void drop_legacy_semantic_tables();

private:
    void create_main_schema();
    void create_vectors_schema();
    void load_sqlite_vec();

    sqlite3* db_ = nullptr;
    DbKind kind_;
};

} // namespace locus

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
    // For Main: just opens + creates skeleton schema.
    // For Vectors: opens connection, loads sqlite-vec, creates `chunks` and a
    //   `meta` table; the `chunk_vectors` virtual table requires a known
    //   embedding dimension and is created lazily by ensure_vectors_schema().
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

    // Vectors-only: read the persisted embedding dimension from the meta
    // table.  Returns 0 if no embeddings have ever been written, or the DB
    // was created by a pre-S4.J build without the meta table.
    int stored_embedding_dim();

    // Vectors-only: ensure `chunk_vectors` exists with the requested dimension.
    // If the table exists with a different dim (model swap), or if `force_wipe`
    // is true, drops + recreates it AND drops `chunks` so the indexer
    // re-chunks every file.  Persists `dim` to the meta table.
    // Returns true if a wipe happened.
    bool ensure_vectors_schema(int dim, bool force_wipe = false);

private:
    void create_main_schema();
    void create_vectors_skeleton();  // chunks + meta only
    void load_sqlite_vec();

    sqlite3* db_ = nullptr;
    DbKind kind_;
};

} // namespace locus

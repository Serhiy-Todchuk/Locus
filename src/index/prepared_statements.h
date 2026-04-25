#pragma once

struct sqlite3_stmt;

namespace locus {

class Database;

// Owns the prepared statements `Indexer` reuses across every file insert /
// update / delete. ctor prepares them all (vectors-only ones stay null when
// `vectors_db == nullptr`), dtor finalises everything. Members are public so
// the indexer can reach them with the same `stmt_*` syntax it always used.
struct IndexerStatements {
    IndexerStatements(Database& main_db, Database* vectors_db);
    ~IndexerStatements();

    IndexerStatements(const IndexerStatements&) = delete;
    IndexerStatements& operator=(const IndexerStatements&) = delete;

    // main.db
    sqlite3_stmt* upsert_file       = nullptr;
    sqlite3_stmt* delete_file       = nullptr;
    sqlite3_stmt* file_id           = nullptr;
    sqlite3_stmt* file_stat         = nullptr;
    sqlite3_stmt* insert_fts        = nullptr;
    sqlite3_stmt* delete_fts        = nullptr;
    sqlite3_stmt* insert_sym        = nullptr;
    sqlite3_stmt* delete_syms       = nullptr;
    sqlite3_stmt* insert_head       = nullptr;
    sqlite3_stmt* delete_heads      = nullptr;

    // vectors.db (all null when semantic search is disabled)
    sqlite3_stmt* insert_chunk      = nullptr;
    sqlite3_stmt* delete_chunks     = nullptr;
    sqlite3_stmt* delete_chunk_vecs = nullptr;
    sqlite3_stmt* file_has_chunks   = nullptr;
};

} // namespace locus

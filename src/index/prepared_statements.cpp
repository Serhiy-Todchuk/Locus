#include "prepared_statements.h"

#include "database.h"

#include <sqlite3.h>

namespace locus {

IndexerStatements::IndexerStatements(Database& main_db, Database* vectors_db)
{
    upsert_file = main_db.prepare(R"(
        INSERT INTO files (path, abs_path, size_bytes, modified_at, ext, is_binary, language, indexed_at)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(path) DO UPDATE SET
            abs_path=?2, size_bytes=?3, modified_at=?4, ext=?5,
            is_binary=?6, language=?7, indexed_at=?8
    )");

    delete_file = main_db.prepare("DELETE FROM files WHERE path = ?1");
    file_id     = main_db.prepare("SELECT id FROM files WHERE path = ?1");
    file_stat   = main_db.prepare(
        "SELECT id, size_bytes, modified_at, is_binary FROM files WHERE path = ?1");

    insert_fts = main_db.prepare("INSERT INTO files_fts (path, content) VALUES (?1, ?2)");
    delete_fts = main_db.prepare("DELETE FROM files_fts WHERE path = ?1");

    insert_sym = main_db.prepare(R"(
        INSERT INTO symbols (file_id, kind, name, line_start, line_end, signature, parent_name)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
    )");
    delete_syms = main_db.prepare("DELETE FROM symbols WHERE file_id = ?1");

    insert_head = main_db.prepare(R"(
        INSERT INTO headings (file_id, level, text, line_number)
        VALUES (?1, ?2, ?3, ?4)
    )");
    delete_heads = main_db.prepare("DELETE FROM headings WHERE file_id = ?1");

    if (vectors_db) {
        insert_chunk = vectors_db->prepare(R"(
            INSERT INTO chunks (file_id, chunk_index, start_line, end_line, content)
            VALUES (?1, ?2, ?3, ?4, ?5)
        )");
        delete_chunks     = vectors_db->prepare("DELETE FROM chunks WHERE file_id = ?1");
        delete_chunk_vecs = vectors_db->prepare(
            "DELETE FROM chunk_vectors WHERE chunk_id IN (SELECT id FROM chunks WHERE file_id = ?1)");
        file_has_chunks   = vectors_db->prepare(
            "SELECT EXISTS(SELECT 1 FROM chunks WHERE file_id = ?1)");
    }
}

IndexerStatements::~IndexerStatements()
{
    auto fin = [](sqlite3_stmt*& s) {
        if (s) { sqlite3_finalize(s); s = nullptr; }
    };
    fin(upsert_file);
    fin(delete_file);
    fin(file_id);
    fin(file_stat);
    fin(insert_fts);
    fin(delete_fts);
    fin(insert_sym);
    fin(delete_syms);
    fin(insert_head);
    fin(delete_heads);
    fin(insert_chunk);
    fin(delete_chunks);
    fin(delete_chunk_vecs);
    fin(file_has_chunks);
}

} // namespace locus

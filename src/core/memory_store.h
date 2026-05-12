#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3_stmt;

namespace locus {

namespace fs = std::filesystem;

class Database;
class EmbeddingWorker;
class Reranker;
struct WorkspaceConfig;

// S4.R -- workspace-scoped persistent memory bank.
//
// Storage layout: `.locus/memory/<id>.md`, one verbatim entry per file with
// YAML frontmatter for metadata + plain markdown body for the content.
// Soft-deleted entries live under `.locus/memory/.deleted/<id>.md` until
// they age out (default 30 days) or get hard-deleted via the Settings UI.
//
// The store is the single source of truth on disk; FTS5 (`memory_fts` in
// main_db) and the vec0 table (`memory_vectors` in vectors_db) are derived
// indexes. Both are rebuilt lazily on construction if rows are missing -
// makes the workspace tolerant of vectors.db deletion or a model/dim swap
// (the latter is handled by `ensure_vectors_schema`, which also drops
// `memory_vectors` so we re-embed cleanly on next open).
class MemoryStore {
public:
    struct Entry {
        std::string              id;            // e.g. "2026-05-12-093427-a3b1"
        std::string              content;       // verbatim, never summarised
        std::vector<std::string> tags;
        std::string              source = "agent";  // user | agent | mined
        bool                     pinned = false;
        // Unix seconds. Created at first add; updated_at bumps on edit;
        // last_used_at bumps when the entry lands in an in-context slot
        // or on a successful search_memory return.
        std::int64_t             created_at   = 0;
        std::int64_t             updated_at   = 0;
        std::int64_t             last_used_at = 0;
    };

    struct SearchHit {
        Entry  entry;
        double score = 0.0;          // higher = better
        // Per-channel contributions, for activity-log debugging and tests.
        double fts_score    = 0.0;   // normalised BM25 contribution
        double vec_score    = 0.0;   // cosine similarity (0..1)
        double recency      = 0.0;   // exp(-age * ln2 / half_life)
        double rerank_score = 0.0;   // cross-encoder logit when applied
    };

    // `vectors_db` / `embedder` / `reranker` may be null; the store falls
    // back to FTS5-only retrieval when semantic search is off.
    MemoryStore(fs::path                memory_dir,
                Database&               main_db,
                Database*               vectors_db,
                EmbeddingWorker*        embedder,
                Reranker*               reranker,
                const WorkspaceConfig&  config);
    ~MemoryStore();

    MemoryStore(const MemoryStore&)            = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    // -- Mutation ------------------------------------------------------------

    // Create a new entry. Embeds + FTS-indexes synchronously, then runs GC.
    // Returns the new entry's id.
    std::string add(std::string              content,
                    std::vector<std::string> tags    = {},
                    bool                     pinned  = false,
                    std::string              source  = "agent");

    // Edit content / tags / pinned in place. Bumps `updated_at` and
    // re-indexes FTS + vector when `content` changes. Returns false if the
    // id doesn't resolve.
    bool update(const std::string&                       id,
                std::optional<std::string>               content,
                std::optional<std::vector<std::string>>  tags,
                std::optional<bool>                      pinned);

    // Soft-delete: moves the .md file under `.deleted/`, drops FTS5 + vec0
    // rows. Returns false if not found.
    bool soft_delete(const std::string& id);

    // Permanent deletion. Removes the entry from FTS5 + vec0, deletes the
    // live `.md` file, and also removes any leftover copy under `.deleted/`
    // so the id is gone for good. Returns false only when the id is not
    // present in any form (live or deleted) -- callers can treat that as
    // already-deleted. Used by integration tests for hermetic cleanup, and
    // by the Phase 2 Settings UI's "delete permanently" button.
    bool hard_delete(const std::string& id);

    // Restore a previously soft-deleted entry (if still within retention).
    bool restore_deleted(const std::string& id);

    // Hard-delete soft-deleted entries older than `retention_days` (default
    // 30). Returns the number purged. Safe to call on workspace open.
    int purge_deleted(int retention_days = 30);

    // Drop oldest unpinned entries beyond `max_entries`. Pinned entries are
    // never affected. Returns the number GC'd.
    int gc();

    // -- Read ----------------------------------------------------------------

    std::optional<Entry> get(const std::string& id) const;

    // Newest-first listing of every live entry. Used by the Settings UI.
    std::vector<Entry> list_all() const;

    // All pinned entries (no cap). Used by the system-prompt assembly first,
    // before list_recent_for_budget consumes the remaining budget.
    std::vector<Entry> list_pinned() const;

    // Most-recently-used unpinned entries, up to whatever fits in
    // `remaining_tokens`. Bumps `last_used_at` on the entries returned.
    std::vector<Entry> list_recent_for_budget(int remaining_tokens);

    // Hybrid retrieval: FTS5 BM25 + cosine (when embedder available) +
    // soft recency factor + optional reranker.
    //   `tag_filter` restricts to entries that carry every supplied tag.
    //   `max_results` caps the returned list size after reranking.
    std::vector<SearchHit> search(const std::string&              query,
                                  int                             max_results,
                                  const std::vector<std::string>& tag_filter = {});

    // Render the always-in-context slot for the system prompt. Includes the
    // section header + every pinned entry + recently-used unpinned entries
    // up to `in_context_budget_tokens` (captured at construction time).
    // Returns empty string when there are no entries to surface so the
    // caller can suppress the section header entirely.
    std::string format_for_system_prompt();

    // -- Introspection -------------------------------------------------------

    std::size_t live_count()    const;
    std::size_t deleted_count() const;
    const fs::path& memory_dir() const { return dir_; }

private:
    // Walks `.locus/memory/*.md`, parses frontmatter, populates the in-memory
    // map. Then reconciles FTS5 + vec0: any entry missing from FTS gets
    // inserted; any entry missing a vector gets embedded (when an embedder
    // is available).
    void load_from_disk();

    void prepare_statements();
    void finalize_statements();

    // Persist a single entry to `.locus/memory/<id>.md`.
    void write_file(const Entry& e) const;

    // Insert / update / delete the per-entry FTS5 row.
    void fts_upsert(const Entry& e);
    void fts_delete(const std::string& id);

    // Embed `text` and write into memory_vectors (mints a memory_keys rowid
    // if absent). No-op when embedder/vectors_db is null. Returns true when
    // the vector was actually written.
    bool vec_embed_and_store(const std::string& id, const std::string& text);
    void vec_delete(const std::string& id);

    // Look up rowid for an existing memory id. Returns 0 if absent.
    std::int64_t lookup_rowid(const std::string& id) const;

    std::string  mint_id(const std::string& source) const;
    void         touch_last_used(const std::string& id, std::int64_t now);

    // Estimate the cost of including this entry in the system-prompt slot.
    // Wraps `TokenCounter::estimate` over a representative format string;
    // budget arithmetic should treat this as the per-entry cost.
    static int   estimate_slot_tokens(const Entry& e);

    fs::path         dir_;
    fs::path         deleted_dir_;
    Database&        main_db_;
    Database*        vectors_db_;
    EmbeddingWorker* embedder_;
    Reranker*        reranker_;
    int              max_entries_;
    int              recency_half_life_days_;
    int              in_context_budget_tokens_;

    mutable std::mutex                               mutex_;
    std::unordered_map<std::string, Entry>           entries_;  // id -> entry

    // Prepared statements -- main_db_
    sqlite3_stmt* stmt_fts_insert_ = nullptr;
    sqlite3_stmt* stmt_fts_delete_ = nullptr;
    sqlite3_stmt* stmt_fts_search_ = nullptr;

    // Prepared statements -- vectors_db_ (null when semantic off)
    sqlite3_stmt* stmt_keys_lookup_ = nullptr;
    sqlite3_stmt* stmt_keys_insert_ = nullptr;
    sqlite3_stmt* stmt_keys_delete_ = nullptr;
    sqlite3_stmt* stmt_vec_insert_  = nullptr;
    sqlite3_stmt* stmt_vec_delete_  = nullptr;
    sqlite3_stmt* stmt_vec_search_  = nullptr;
};

} // namespace locus

#pragma once

#include "file_watcher.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct sqlite3_stmt;
typedef struct TSParser TSParser;
typedef struct TSLanguage TSLanguage;

namespace locus {

namespace fs = std::filesystem;

class Database;
class ExtractorRegistry;
struct WorkspaceConfig;

// Walks the workspace, reads files, and populates the FTS5, symbols, and
// headings tables.  Handles both initial full traversal and incremental
// updates driven by FileWatcher events.
class Indexer {
public:
    // `vectors_db` is optional: pass null when semantic search is disabled
    // (no chunks / vectors will be written).
    Indexer(Database& main_db, Database* vectors_db,
            const fs::path& root, const WorkspaceConfig& config,
            const ExtractorRegistry& extractors);
    ~Indexer();

    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    // Full initial traversal — indexes every non-excluded file.
    void build_initial();

    // Process a batch of file-watcher events (incremental update).
    void process_events(const std::vector<FileEvent>& events);

    struct Stats {
        int files_total    = 0;
        int files_indexed  = 0;   // text files with FTS content
        int files_binary   = 0;
        int symbols_total  = 0;
        int headings_total = 0;
        int chunks_total   = 0;
    };
    const Stats& stats() const { return stats_; }

    // Called with chunk IDs after each file is chunked (for embedding queue).
    std::function<void(std::vector<int64_t>)> on_chunks_created;

    // Activity hook — summary + detail, thread-safe handler required.
    // Fired after build_initial completes and after each process_events batch.
    std::function<void(const std::string&, const std::string&)> on_activity;

    // Progress hook — fires per file while `build_initial` or `process_events`
    // is running. `done == total` signals end-of-batch. Fires on the thread
    // driving the indexer; handler must be thread-safe.
    std::function<void(int done, int total)> on_progress;

private:
    // Index a single file (upsert files row, FTS5, symbols, headings).
    void index_file(const fs::path& rel_path);

    // Remove all index data for a file.
    void remove_file(const fs::path& rel_path);

    // Detection helpers
    bool is_excluded(const fs::path& rel_path) const;
    static bool is_binary_content(const char* data, size_t len);
    static std::string detect_language(const std::string& ext);

    // Content extraction
    void insert_fts(const std::string& rel_path_str, const std::string& content);
    void delete_fts(const std::string& rel_path_str);
    void insert_headings(int64_t file_id,
                         const std::vector<struct ExtractedHeading>& headings);
    void extract_symbols(int64_t file_id, const std::string& content,
                         const std::string& language,
                         std::vector<struct SymbolSpan>& out_spans);

    // Tree-sitter helpers
    void init_tree_sitter();
    const TSLanguage* ts_language_for(const std::string& language) const;

    Database& main_db_;
    Database* vectors_db_;  // nullable — null when semantic disabled
    fs::path root_;
    const WorkspaceConfig& config_;
    const ExtractorRegistry& extractors_;
    Stats stats_;

    // Prepared statements (owned, finalised in destructor)
    sqlite3_stmt* stmt_upsert_file_  = nullptr;
    sqlite3_stmt* stmt_delete_file_  = nullptr;
    sqlite3_stmt* stmt_file_id_      = nullptr;
    sqlite3_stmt* stmt_insert_fts_   = nullptr;
    sqlite3_stmt* stmt_delete_fts_   = nullptr;
    sqlite3_stmt* stmt_insert_sym_   = nullptr;
    sqlite3_stmt* stmt_delete_syms_  = nullptr;
    sqlite3_stmt* stmt_insert_head_  = nullptr;
    sqlite3_stmt* stmt_delete_heads_ = nullptr;
    sqlite3_stmt* stmt_insert_chunk_   = nullptr;
    sqlite3_stmt* stmt_delete_chunks_  = nullptr;
    sqlite3_stmt* stmt_delete_chunk_vecs_ = nullptr;

    // Tree-sitter
    TSParser* ts_parser_ = nullptr;
    std::unordered_map<std::string, const TSLanguage*> ts_languages_;
};

} // namespace locus

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3_stmt;

namespace locus {

namespace fs = std::filesystem;

class Database;

// -- Result types -------------------------------------------------------------

struct SearchResult {
    std::string path;
    int line = 0;           // first matching line (0 if unknown)
    std::string snippet;    // context around match
    double score = 0.0;     // BM25 rank (lower = better match in SQLite FTS5)
};

struct SymbolResult {
    std::string path;
    std::string name;
    std::string kind;       // "function", "class", "struct", "method", etc.
    std::string language;
    int line_start = 0;
    int line_end = 0;
    std::string signature;
    std::string parent_name;
};

struct OutlineEntry {
    enum Type { Heading, Symbol };
    Type type;
    int line = 0;
    int level = 0;          // heading level (1-6), or 0 for symbols
    std::string text;       // heading text or symbol name
    std::string kind;       // symbol kind (empty for headings)
    std::string signature;  // symbol signature (empty for headings)
};

struct FileEntry {
    std::string path;
    bool is_directory = false;
    int64_t size_bytes = 0;
    int64_t modified_at = 0;
    std::string ext;
    std::string language;
    bool is_binary = false;
};

struct SearchOptions {
    int max_results = 20;
};

// -- IndexQuery ---------------------------------------------------------------

// Read-only query interface over the workspace index.
// Wraps SQLite prepared statements for efficient repeated use.
class IndexQuery {
public:
    explicit IndexQuery(Database& db);
    ~IndexQuery();

    IndexQuery(const IndexQuery&) = delete;
    IndexQuery& operator=(const IndexQuery&) = delete;

    // FTS5 BM25 full-text search.
    std::vector<SearchResult> search_text(const std::string& query,
                                          const SearchOptions& opts = {}) const;

    // Symbol lookup with prefix match. kind and language are optional filters.
    std::vector<SymbolResult> search_symbols(const std::string& name,
                                             const std::string& kind = "",
                                             const std::string& language = "") const;

    // Headings + symbols for a single file, sorted by line number.
    std::vector<OutlineEntry> get_file_outline(const std::string& path) const;

    // Directory listing from the files table. depth=0 means immediate children only.
    std::vector<FileEntry> list_directory(const std::string& path, int depth = 0) const;

private:
    Database& db_;

    // Prepared statements (owned, finalized in destructor)
    sqlite3_stmt* stmt_search_text_ = nullptr;
    sqlite3_stmt* stmt_search_symbols_all_ = nullptr;
    sqlite3_stmt* stmt_outline_headings_ = nullptr;
    sqlite3_stmt* stmt_outline_symbols_ = nullptr;
    sqlite3_stmt* stmt_list_dir_ = nullptr;
};

} // namespace locus

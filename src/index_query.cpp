#include "index_query.h"
#include "database.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace locus {

// -- Helpers ------------------------------------------------------------------

// Extract a non-null text column, returning empty string on NULL.
static std::string col_text(sqlite3_stmt* stmt, int col)
{
    auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? t : "";
}

// Find the 1-based line number of the first occurrence of `term` in `content`.
// Returns 0 if not found.
static int find_match_line(const std::string& content, const std::string& term)
{
    // Case-insensitive search for the first query term
    auto it_content = content.begin();
    auto it_end = content.end();

    // Simple case-insensitive find
    std::string lower_content(content.size(), '\0');
    std::string lower_term(term.size(), '\0');
    std::transform(content.begin(), content.end(), lower_content.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(term.begin(), term.end(), lower_term.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto pos = lower_content.find(lower_term);
    if (pos == std::string::npos) return 0;

    // Count newlines before pos
    int line = 1;
    for (size_t i = 0; i < pos; ++i) {
        if (content[i] == '\n') ++line;
    }
    return line;
}

// Extract a snippet: the line containing `pos` plus one line of context each side.
static std::string extract_snippet(const std::string& content, int line, int context_lines = 1)
{
    if (line <= 0 || content.empty()) return {};

    // Find line boundaries
    std::vector<size_t> line_starts;
    line_starts.push_back(0);
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') line_starts.push_back(i + 1);
    }

    int start_line = std::max(1, line - context_lines);
    int end_line = std::min(static_cast<int>(line_starts.size()), line + context_lines);

    size_t start_pos = line_starts[start_line - 1];
    size_t end_pos = (end_line < static_cast<int>(line_starts.size()))
                         ? line_starts[end_line]
                         : content.size();

    std::string snippet = content.substr(start_pos, end_pos - start_pos);
    // Trim trailing newline/whitespace
    while (!snippet.empty() && (snippet.back() == '\n' || snippet.back() == '\r'))
        snippet.pop_back();

    // Cap snippet length
    if (snippet.size() > 500) {
        snippet.resize(500);
        snippet += "...";
    }
    return snippet;
}

// -- Construction / Destruction -----------------------------------------------

IndexQuery::IndexQuery(Database& main_db, Database* vectors_db)
    : main_db_(main_db)
    , vectors_db_(vectors_db)
{
    // FTS5 search with BM25 ranking. Join with files to get abs_path for content reading.
    // bm25() returns negative values where more negative = better match.
    stmt_search_text_ = main_db_.prepare(R"(
        SELECT f.path, bm25(files_fts) AS rank, f.abs_path
        FROM files_fts
        JOIN files f ON f.path = files_fts.path
        WHERE files_fts MATCH ?1
        ORDER BY rank
        LIMIT ?2
    )");

    // Symbol search with prefix match. Dynamic WHERE built per-call would defeat
    // the point of prepared statements, so we use a single query that always
    // filters on name with LIKE and optionally on kind/language via CASE tricks.
    // Simpler approach: prepare a generous query; filter in C++ for kind/language.
    stmt_search_symbols_all_ = main_db_.prepare(R"(
        SELECT f.path, s.name, s.kind, f.language, s.line_start, s.line_end,
               s.signature, s.parent_name
        FROM symbols s
        JOIN files f ON f.id = s.file_id
        WHERE s.name LIKE ?1
        ORDER BY s.name, f.path
        LIMIT 200
    )");

    stmt_outline_headings_ = main_db_.prepare(R"(
        SELECT h.level, h.text, h.line_number
        FROM headings h
        JOIN files f ON f.id = h.file_id
        WHERE f.path = ?1
        ORDER BY h.line_number
    )");

    stmt_outline_symbols_ = main_db_.prepare(R"(
        SELECT s.kind, s.name, s.line_start, s.signature
        FROM symbols s
        JOIN files f ON f.id = s.file_id
        WHERE f.path = ?1
        ORDER BY s.line_start
    )");

    stmt_list_dir_ = main_db_.prepare(R"(
        SELECT path, size_bytes, modified_at, ext, language, is_binary
        FROM files
        ORDER BY path
    )");

    // Semantic search: vectors.db knows chunks + embeddings but NOT file paths.
    // We resolve file_id -> path in a second lookup on main_db_.
    if (vectors_db_) {
        stmt_search_semantic_ = vectors_db_->prepare(R"(
            SELECT c.content, c.start_line, c.file_id, cv.distance
            FROM chunk_vectors cv
            JOIN chunks c ON c.id = cv.chunk_id
            WHERE cv.embedding MATCH ?1 AND k = ?2
            ORDER BY cv.distance
        )");

        stmt_path_by_file_id_ = main_db_.prepare("SELECT path FROM files WHERE id = ?1");
    }

    spdlog::trace("IndexQuery initialised");
}

IndexQuery::~IndexQuery()
{
    auto finalize = [](sqlite3_stmt*& s) {
        if (s) { sqlite3_finalize(s); s = nullptr; }
    };
    finalize(stmt_search_text_);
    finalize(stmt_search_symbols_all_);
    finalize(stmt_outline_headings_);
    finalize(stmt_outline_symbols_);
    finalize(stmt_list_dir_);
    finalize(stmt_path_by_file_id_);
    finalize(stmt_search_semantic_);

    spdlog::trace("IndexQuery destroyed");
}

// -- search_text --------------------------------------------------------------

std::vector<SearchResult> IndexQuery::search_text(const std::string& query,
                                                  const SearchOptions& opts) const
{
    spdlog::trace("search_text: query='{}', max_results={}", query, opts.max_results);

    std::vector<SearchResult> results;

    sqlite3_reset(stmt_search_text_);
    sqlite3_bind_text(stmt_search_text_, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_search_text_, 2, opts.max_results);

    while (sqlite3_step(stmt_search_text_) == SQLITE_ROW) {
        SearchResult r;
        r.path = col_text(stmt_search_text_, 0);
        r.score = sqlite3_column_double(stmt_search_text_, 1);
        std::string abs_path = col_text(stmt_search_text_, 2);

        // Read file content to find match line and snippet.
        // For performance, read only first 64 KB.
        std::string content;
        {
            std::ifstream f(abs_path, std::ios::binary);
            if (f.is_open()) {
                content.resize(65536);
                f.read(content.data(), static_cast<std::streamsize>(content.size()));
                content.resize(static_cast<size_t>(f.gcount()));
            }
        }

        // Extract first query term for line finding (strip FTS5 operators)
        std::string first_term = query;
        // Take first word, strip quotes/operators
        auto sp = first_term.find(' ');
        if (sp != std::string::npos) first_term.resize(sp);
        // Strip leading NOT/OR
        if (first_term == "NOT" || first_term == "OR") first_term = query.substr(sp + 1);

        r.line = find_match_line(content, first_term);
        r.snippet = extract_snippet(content, r.line);

        results.push_back(std::move(r));
    }

    spdlog::trace("search_text: {} results", results.size());
    return results;
}

// -- search_symbols -----------------------------------------------------------

std::vector<SymbolResult> IndexQuery::search_symbols(const std::string& name,
                                                     const std::string& kind,
                                                     const std::string& language) const
{
    spdlog::trace("search_symbols: name='{}', kind='{}', language='{}'", name, kind, language);

    std::vector<SymbolResult> results;

    // Prefix match: name%
    std::string pattern = name + "%";

    sqlite3_reset(stmt_search_symbols_all_);
    sqlite3_bind_text(stmt_search_symbols_all_, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt_search_symbols_all_) == SQLITE_ROW) {
        SymbolResult r;
        r.path        = col_text(stmt_search_symbols_all_, 0);
        r.name        = col_text(stmt_search_symbols_all_, 1);
        r.kind        = col_text(stmt_search_symbols_all_, 2);
        r.language    = col_text(stmt_search_symbols_all_, 3);
        r.line_start  = sqlite3_column_int(stmt_search_symbols_all_, 4);
        r.line_end    = sqlite3_column_int(stmt_search_symbols_all_, 5);
        r.signature   = col_text(stmt_search_symbols_all_, 6);
        r.parent_name = col_text(stmt_search_symbols_all_, 7);

        // Apply optional filters
        if (!kind.empty() && r.kind != kind) continue;
        if (!language.empty() && r.language != language) continue;

        results.push_back(std::move(r));
    }

    spdlog::trace("search_symbols: {} results", results.size());
    return results;
}

// -- get_file_outline ---------------------------------------------------------

std::vector<OutlineEntry> IndexQuery::get_file_outline(const std::string& path) const
{
    spdlog::trace("get_file_outline: path='{}'", path);

    std::vector<OutlineEntry> entries;

    // Collect headings
    sqlite3_reset(stmt_outline_headings_);
    sqlite3_bind_text(stmt_outline_headings_, 1, path.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt_outline_headings_) == SQLITE_ROW) {
        OutlineEntry e;
        e.type = OutlineEntry::Heading;
        e.level = sqlite3_column_int(stmt_outline_headings_, 0);
        e.text = col_text(stmt_outline_headings_, 1);
        e.line = sqlite3_column_int(stmt_outline_headings_, 2);
        entries.push_back(std::move(e));
    }

    // Collect symbols
    sqlite3_reset(stmt_outline_symbols_);
    sqlite3_bind_text(stmt_outline_symbols_, 1, path.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt_outline_symbols_) == SQLITE_ROW) {
        OutlineEntry e;
        e.type = OutlineEntry::Symbol;
        e.kind = col_text(stmt_outline_symbols_, 0);
        e.text = col_text(stmt_outline_symbols_, 1);
        e.line = sqlite3_column_int(stmt_outline_symbols_, 2);
        e.signature = col_text(stmt_outline_symbols_, 3);
        entries.push_back(std::move(e));
    }

    // Sort by line number
    std::sort(entries.begin(), entries.end(),
              [](const OutlineEntry& a, const OutlineEntry& b) { return a.line < b.line; });

    spdlog::trace("get_file_outline: {} entries", entries.size());
    return entries;
}

// -- list_directory -----------------------------------------------------------

std::vector<FileEntry> IndexQuery::list_directory(const std::string& path, int depth) const
{
    spdlog::trace("list_directory: path='{}', depth={}", path, depth);

    std::vector<FileEntry> results;

    // Normalize prefix: empty or "." = root, otherwise ensure trailing /
    std::string prefix = path;
    if (prefix == "." || prefix == "./") prefix.clear();
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    // We scan all files and filter in C++. For workspaces up to tens of
    // thousands of files this is fast enough (< 100ms).
    sqlite3_reset(stmt_list_dir_);

    // Track directories we've already emitted
    std::vector<std::string> seen_dirs;

    while (sqlite3_step(stmt_list_dir_) == SQLITE_ROW) {
        std::string file_path = col_text(stmt_list_dir_, 0);

        // Must be under the requested prefix
        if (!prefix.empty() && file_path.find(prefix) != 0) continue;
        // For root listing, no prefix filter needed

        // Get the relative part after prefix
        std::string rel = prefix.empty() ? file_path : file_path.substr(prefix.size());

        // Count depth (number of / separators in rel)
        int rel_depth = 0;
        for (char c : rel) {
            if (c == '/') ++rel_depth;
        }

        // If beyond requested depth, emit directory entries instead
        if (rel_depth > depth) {
            // Extract the directory at the target depth
            size_t pos = 0;
            for (int d = 0; d <= depth; ++d) {
                auto slash = rel.find('/', pos);
                if (slash == std::string::npos) break;
                pos = slash + 1;
            }
            if (pos > 0) {
                std::string dir_path = prefix + rel.substr(0, pos - 1);
                // Check if we already emitted this directory
                bool seen = false;
                for (auto& sd : seen_dirs) {
                    if (sd == dir_path) { seen = true; break; }
                }
                if (!seen) {
                    seen_dirs.push_back(dir_path);
                    FileEntry de;
                    de.path = dir_path;
                    de.is_directory = true;
                    results.push_back(std::move(de));
                }
            }
            continue;
        }

        FileEntry e;
        e.path = file_path;
        e.is_directory = false;
        e.size_bytes = sqlite3_column_int64(stmt_list_dir_, 1);
        e.modified_at = sqlite3_column_int64(stmt_list_dir_, 2);
        e.ext = col_text(stmt_list_dir_, 3);
        e.language = col_text(stmt_list_dir_, 4);
        e.is_binary = sqlite3_column_int(stmt_list_dir_, 5) != 0;
        results.push_back(std::move(e));
    }

    // Sort: directories first, then by path
    std::sort(results.begin(), results.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.is_directory != b.is_directory) return a.is_directory > b.is_directory;
        return a.path < b.path;
    });

    spdlog::trace("list_directory: {} entries", results.size());
    return results;
}

// -- search_semantic ----------------------------------------------------------

std::vector<SearchResult> IndexQuery::search_semantic(
    const std::vector<float>& query_embedding,
    const SearchOptions& opts) const
{
    spdlog::trace("search_semantic: dim={}, max_results={}",
                  query_embedding.size(), opts.max_results);

    std::vector<SearchResult> results;

    if (!stmt_search_semantic_) {
        spdlog::trace("search_semantic: vectors DB not present, returning empty");
        return results;
    }

    // Step 1: ask vectors.db for the top-K nearest chunks.
    struct PendingHit { std::string snippet; int line; int64_t file_id; double distance; };
    std::vector<PendingHit> hits;
    hits.reserve(opts.max_results);

    sqlite3_reset(stmt_search_semantic_);
    sqlite3_bind_blob(stmt_search_semantic_, 1,
                      query_embedding.data(),
                      static_cast<int>(query_embedding.size() * sizeof(float)),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_search_semantic_, 2, opts.max_results);

    while (sqlite3_step(stmt_search_semantic_) == SQLITE_ROW) {
        PendingHit h;
        h.snippet  = col_text(stmt_search_semantic_, 0);
        h.line     = sqlite3_column_int(stmt_search_semantic_, 1);
        h.file_id  = sqlite3_column_int64(stmt_search_semantic_, 2);
        h.distance = sqlite3_column_double(stmt_search_semantic_, 3);
        hits.push_back(std::move(h));
    }

    // Step 2: resolve file_id -> path via main_db_.
    for (auto& h : hits) {
        SearchResult r;
        r.line  = h.line;
        r.score = 1.0 - h.distance;  // cosine similarity

        sqlite3_reset(stmt_path_by_file_id_);
        sqlite3_bind_int64(stmt_path_by_file_id_, 1, h.file_id);
        if (sqlite3_step(stmt_path_by_file_id_) == SQLITE_ROW) {
            r.path = col_text(stmt_path_by_file_id_, 0);
        } else {
            // Orphan chunk: main DB no longer has this file. Skip.
            continue;
        }

        if (h.snippet.size() > 500) {
            h.snippet.resize(500);
            h.snippet += "...";
        }
        r.snippet = std::move(h.snippet);

        results.push_back(std::move(r));
    }

    spdlog::trace("search_semantic: {} results", results.size());
    return results;
}

// -- search_hybrid ------------------------------------------------------------

std::vector<SearchResult> IndexQuery::search_hybrid(
    const std::string& query_text,
    const std::vector<float>& query_embedding,
    const SearchOptions& opts) const
{
    spdlog::trace("search_hybrid: query='{}', max_results={}", query_text, opts.max_results);

    // Run both searches with 2x results for better fusion
    SearchOptions wider_opts;
    wider_opts.max_results = opts.max_results * 2;

    auto bm25_results = search_text(query_text, wider_opts);
    auto vec_results = search_semantic(query_embedding, wider_opts);

    // Reciprocal Rank Fusion (k=60)
    constexpr double k = 60.0;

    // Map: path -> combined RRF score + best result data
    struct RrfEntry {
        SearchResult result;
        double rrf_score = 0.0;
    };
    std::unordered_map<std::string, RrfEntry> merged;

    // Add BM25 results
    for (int i = 0; i < static_cast<int>(bm25_results.size()); ++i) {
        auto& r = bm25_results[i];
        std::string key = r.path + ":" + std::to_string(r.line);
        auto& entry = merged[key];
        entry.rrf_score += 1.0 / (k + i + 1);
        if (entry.result.path.empty()) {
            entry.result = r;
        }
    }

    // Add semantic results
    for (int i = 0; i < static_cast<int>(vec_results.size()); ++i) {
        auto& r = vec_results[i];
        std::string key = r.path + ":" + std::to_string(r.line);
        auto& entry = merged[key];
        entry.rrf_score += 1.0 / (k + i + 1);
        if (entry.result.path.empty()) {
            entry.result = r;
        }
    }

    // Sort by RRF score descending
    std::vector<std::pair<std::string, RrfEntry>> sorted_entries(
        merged.begin(), merged.end());
    std::sort(sorted_entries.begin(), sorted_entries.end(),
              [](const auto& a, const auto& b) {
                  return a.second.rrf_score > b.second.rrf_score;
              });

    // Take top results
    std::vector<SearchResult> results;
    int limit = std::min(opts.max_results, static_cast<int>(sorted_entries.size()));
    for (int i = 0; i < limit; ++i) {
        auto& entry = sorted_entries[i].second;
        entry.result.score = entry.rrf_score;
        results.push_back(std::move(entry.result));
    }

    spdlog::trace("search_hybrid: {} results (from {} BM25 + {} semantic)",
                  results.size(), bm25_results.size(), vec_results.size());
    return results;
}

} // namespace locus

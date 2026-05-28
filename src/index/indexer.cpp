#include "indexer.h"
#include "chunker.h"
#include "../core/database.h"
#include "extractors/extractor_registry.h"
#include "extractors/text_extractor.h"
#include "gitignore.h"
#include "glob_match.h"
#include "../core/workspace.h"
#include "../core/log_channels.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <tree_sitter/api.h>

#include <chrono>
#include <fstream>

namespace locus {

// -- Helpers ------------------------------------------------------------------

static std::string read_file_content(const fs::path& abs_path, int max_size_kb)
{
    std::error_code ec;
    auto file_size = fs::file_size(abs_path, ec);
    if (ec) return {};

    if (max_size_kb > 0 && file_size > static_cast<uintmax_t>(max_size_kb) * 1024) {
        log_fs()->trace("Skipping large file ({} KB): {}", file_size / 1024, abs_path.string());
        return {};
    }

    std::ifstream f(abs_path, std::ios::binary);
    if (!f.is_open()) return {};

    std::string content;
    content.resize(static_cast<size_t>(file_size));
    f.read(content.data(), static_cast<std::streamsize>(file_size));
    content.resize(static_cast<size_t>(f.gcount()));
    return content;
}

static int64_t unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// -- Indexer ------------------------------------------------------------------

Indexer::Indexer(Database& main_db, Database* vectors_db,
                 const fs::path& root, const WorkspaceConfig& config,
                 const ExtractorRegistry& extractors)
    : main_db_(main_db)
    , vectors_db_(vectors_db)
    , root_(root)
    , config_(config)
    , extractors_(extractors)
    , stmts_(main_db, vectors_db)
{
    register_builtin_symbol_extractors(symbol_extractors_);
    if (config_.index.respect_gitignore) {
        // Universal skip list: directories that are essentially never
        // sources of useful .gitignore rules and that bloat the walker if
        // descended (esp. `build/` for repos that vendor third-party src
        // via FetchContent -- each pulled grammar/library can ship its own
        // .gitignore, which we'd otherwise sum into the thousands).
        gitignore_patterns_ = load_workspace_gitignore(
            root_,
            {".git", ".locus", "node_modules", "build", "dist", "out", "target"});
    }
    log_fs()->trace("Indexer initialised ({} gitignore patterns)",
                  gitignore_patterns_.size());
}

Indexer::~Indexer()
{
    // S6.18 Task A.6 -- fold the WAL back into the main DB on workspace close
    // so successive sessions don't keep accumulating WAL bytes between clean
    // shutdowns. PASSIVE checkpoint never blocks; opportunistic.
    main_db_.wal_checkpoint_passive("indexer shutdown");
    if (vectors_db_) {
        vectors_db_->wal_checkpoint_passive("indexer shutdown vectors");
    }
    log_fs()->trace("Indexer destroyed");
}

// -- Language & binary detection ----------------------------------------------

std::string Indexer::detect_language(const std::string& ext)
{
    static const std::unordered_map<std::string, std::string> map = {
        {".c",    "c"},
        {".h",    "cpp"},       // treat .h as C++ (superset)
        {".cpp",  "cpp"},
        {".cc",   "cpp"},
        {".cxx",  "cpp"},
        {".hpp",  "cpp"},
        {".hxx",  "cpp"},
        {".py",   "python"},
        {".pyw",  "python"},
        {".js",   "javascript"},
        {".mjs",  "javascript"},
        {".jsx",  "javascript"},
        {".ts",   "typescript"},
        {".tsx",  "typescript"},
        {".go",   "go"},
        {".rs",   "rust"},
        {".java", "java"},
        {".cs",   "csharp"},
        {".rb",   "ruby"},
        {".php",  "php"},
        {".phtml","php"},
        {".swift","swift"},
        {".kt",   "kotlin"},
        {".kts",  "kotlin"},
        {".md",   "markdown"},
        {".markdown", "markdown"},
        {".txt",  "text"},
        {".json", "json"},
        {".yaml", "yaml"},
        {".yml",  "yaml"},
        {".toml", "toml"},
        {".xml",  "xml"},
        {".html", "html"},
        {".htm",  "html"},
        {".css",  "css"},
        {".sql",  "sql"},
        {".sh",   "bash"},
        {".bash", "bash"},
        {".bat",  "batch"},
        {".ps1",  "powershell"},
        {".cmake","cmake"},
    };

    auto it = map.find(ext);
    return (it != map.end()) ? it->second : "";
}

bool Indexer::is_binary_content(const char* data, size_t len)
{
    // Check first 8 KB for null bytes
    size_t check = std::min(len, size_t(8192));
    for (size_t i = 0; i < check; ++i) {
        if (data[i] == '\0') return true;
    }
    return false;
}

bool Indexer::is_excluded(const fs::path& rel_path) const
{
    std::string rel_str = rel_path.string();
    std::replace(rel_str.begin(), rel_str.end(), '\\', '/');
    for (auto& pattern : config_.index.exclude_patterns) {
        if (glob_match(pattern, rel_str)) return true;
    }
    // S4.L -- treat the rel_str as a file path for gitignore matching. The
    // indexer never feeds directories through index_file, so directory_only
    // patterns scoped to a file path naturally don't match -- but the
    // accompanying `<dir>/**` pattern emitted by parse_gitignore_text catches
    // every file underneath, which is the behaviour we want.
    if (!gitignore_patterns_.empty()
        && gitignore_match(gitignore_patterns_, rel_str, /*is_directory=*/false))
        return true;
    return false;
}

int Indexer::reload_gitignore()
{
    if (!config_.index.respect_gitignore) {
        gitignore_patterns_.clear();
        return 0;
    }

    gitignore_patterns_ = load_workspace_gitignore(
        root_,
        {".git", ".locus", "node_modules", "build", "dist", "out", "target"});

    int dropped = reconcile_excluded_files();
    spdlog::info("gitignore: reloaded ({} patterns), dropped {} now-excluded path(s)",
                 gitignore_patterns_.size(), dropped);
    return dropped;
}

int Indexer::reconcile_excluded_files()
{
    // Walk the existing index and drop anything the current pattern set
    // (explicit excludes + gitignore) excludes. Used at workspace open to
    // clean up rows left over from a prior session whose excludes were
    // narrower, and called from reload_gitignore() to apply mid-session
    // .gitignore edits. Cheap (single SELECT) compared to a full re-scan.
    std::vector<std::string> to_drop;
    if (auto* s = main_db_.prepare("SELECT path FROM files"); s) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
            if (!p) continue;
            std::string rel = p;
            if (is_excluded(rel))
                to_drop.push_back(rel);
        }
        sqlite3_finalize(s);
    }

    if (!to_drop.empty()) {
        main_db_.exec("BEGIN TRANSACTION");
        if (vectors_db_) vectors_db_->exec("BEGIN TRANSACTION");
        for (const auto& rel : to_drop) {
            remove_file(rel);
        }
        main_db_.exec("COMMIT");
        if (vectors_db_) vectors_db_->exec("COMMIT");
    }
    return static_cast<int>(to_drop.size());
}

int Indexer::reconcile_nonexistent_files()
{
    std::vector<std::string> to_drop;
    if (auto* s = main_db_.prepare("SELECT path FROM files"); s) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
            if (!p) continue;
            std::string rel = p;
            std::error_code ec;
            fs::path abs = root_ / fs::path(rel);
            // is_regular_file returns false for directories, missing paths,
            // and symlinks-to-nothing -- exactly the set we want to evict.
            if (!fs::is_regular_file(abs, ec))
                to_drop.push_back(rel);
        }
        sqlite3_finalize(s);
    }

    if (!to_drop.empty()) {
        main_db_.exec("BEGIN TRANSACTION");
        if (vectors_db_) vectors_db_->exec("BEGIN TRANSACTION");
        for (const auto& rel : to_drop) {
            remove_file(rel);
            spdlog::info("Dropped stale file row: {} (not a regular file)", rel);
        }
        main_db_.exec("COMMIT");
        if (vectors_db_) vectors_db_->exec("COMMIT");
    }
    return static_cast<int>(to_drop.size());
}

// -- Lazy chunk_vectors-delete preparation (S6.18 Task A.1) ------------------

// `delete_chunk_vecs` references the vec0 `chunk_vectors` table. If that
// table doesn't exist yet (brand-new workspace or post-wipe), the prepare
// inside IndexerStatements is deferred. This helper materialises the table
// (cheap: vec0 CREATE without dim cost is fine since the prepare side
// needs the schema) and the prepared statement on first use. Returns the
// statement, or nullptr when the caller should skip the DELETE (no
// vectors_db_, or the table genuinely cannot be created right now).
static sqlite3_stmt* ensure_delete_chunk_vecs_stmt(Database* vectors_db,
                                                   IndexerStatements& stmts)
{
    if (!vectors_db) return nullptr;
    if (stmts.delete_chunk_vecs) return stmts.delete_chunk_vecs;
    if (!vectors_db->chunk_vectors_present()) {
        // No table to DELETE from -- nothing to do. The first writer (the
        // embedding worker) will mint it; until then, our DELETE would be a
        // no-op anyway.
        return nullptr;
    }
    stmts.delete_chunk_vecs = vectors_db->prepare(
        "DELETE FROM chunk_vectors WHERE chunk_id IN "
        "(SELECT id FROM chunks WHERE file_id = ?1)");
    return stmts.delete_chunk_vecs;
}

// -- FTS5 operations ----------------------------------------------------------

void Indexer::insert_fts(const std::string& rel_path_str, const std::string& content)
{
    sqlite3_reset(stmts_.insert_fts);
    sqlite3_bind_text(stmts_.insert_fts, 1, rel_path_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmts_.insert_fts, 2, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmts_.insert_fts);
}

void Indexer::delete_fts(const std::string& rel_path_str)
{
    sqlite3_reset(stmts_.delete_fts);
    sqlite3_bind_text(stmts_.delete_fts, 1, rel_path_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmts_.delete_fts);
}

// -- Heading insertion (from extractor results) --------------------------------

void Indexer::insert_headings(int64_t file_id,
                              const std::vector<ExtractedHeading>& headings)
{
    for (const auto& h : headings) {
        sqlite3_reset(stmts_.insert_head);
        sqlite3_bind_int64(stmts_.insert_head, 1, file_id);
        sqlite3_bind_int(stmts_.insert_head, 2, h.level);
        sqlite3_bind_text(stmts_.insert_head, 3, h.text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmts_.insert_head, 4, h.line_number);
        sqlite3_step(stmts_.insert_head);

        ++stats_.headings_total;
    }
}

// -- Symbol extraction (Tree-sitter) ------------------------------------------

void Indexer::extract_symbols(int64_t file_id, const std::string& content,
                              const std::string& language,
                              std::vector<SymbolSpan>& out_spans)
{
    const TSLanguage* ts_lang = ts_registry_.language_for_name(language);
    if (!ts_lang) return;

    const ISymbolExtractor* extractor = symbol_extractors_.find(language);
    if (!extractor) return;

    TSParser* parser = ts_registry_.parser();
    ts_parser_set_language(parser, ts_lang);
    TSTree* tree = ts_parser_parse_string(parser, nullptr,
                                          content.c_str(),
                                          static_cast<uint32_t>(content.size()));
    if (!tree) {
        spdlog::warn("Tree-sitter parse failed for language '{}'", language);
        return;
    }

    auto symbols = extractor->extract(ts_tree_root_node(tree), content);

    for (const auto& sym : symbols) {
        sqlite3_reset(stmts_.insert_sym);
        sqlite3_bind_int64(stmts_.insert_sym, 1, file_id);
        sqlite3_bind_text(stmts_.insert_sym, 2, sym.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmts_.insert_sym, 3, sym.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmts_.insert_sym, 4, sym.line_start);
        sqlite3_bind_int(stmts_.insert_sym, 5, sym.line_end);
        sqlite3_bind_text(stmts_.insert_sym, 6, sym.signature.c_str(), -1, SQLITE_TRANSIENT);
        if (sym.parent_name.empty())
            sqlite3_bind_null(stmts_.insert_sym, 7);
        else
            sqlite3_bind_text(stmts_.insert_sym, 7, sym.parent_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmts_.insert_sym);

        ++stats_.symbols_total;
        out_spans.push_back({sym.line_start, sym.line_end});

        log_fs()->trace("Symbol: {} {} '{}' lines {}-{}", sym.kind, language, sym.name,
                      sym.line_start, sym.line_end);
    }

    ts_tree_delete(tree);
}

// -- Single file indexing -----------------------------------------------------

void Indexer::index_file(const fs::path& rel_path)
{
    fs::path abs_path = root_ / rel_path;
    std::string rel_str = rel_path.string();
    std::replace(rel_str.begin(), rel_str.end(), '\\', '/');

    std::error_code ec;
    // Reject anything that isn't a regular file -- directories, symlinks,
    // etc. The initial-scan path already filters via
    // `entry.is_regular_file()` in the recursive_directory_iterator loop,
    // but watcher events for newly-created directories (most commonly
    // `.locus/` itself when opening a fresh workspace) reach this function
    // directly. fs::file_size() on a directory on Windows returns 4096
    // instead of failing, so without this check we'd insert a bogus file
    // row for every created directory.
    if (!fs::is_regular_file(abs_path, ec)) return;

    auto fsize = fs::file_size(abs_path, ec);
    if (ec) return;

    auto ftime = fs::last_write_time(abs_path, ec);
    // Convert to unix timestamp
    auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    int64_t mtime = std::chrono::duration_cast<std::chrono::seconds>(
        sctp.time_since_epoch()).count();

    // Fast path -- skip any file whose size+mtime match what's already in the
    // index. Saves re-chunking + re-embedding every chunk on every startup.
    // When semantic search is enabled, also require that chunks already exist
    // for this file (guards against a wiped or freshly-enabled vectors.db).
    // Stats are still bumped so the final "Index: N files" summary reflects
    // the full on-disk state, not just files touched this session.
    sqlite3_reset(stmts_.file_stat);
    sqlite3_bind_text(stmts_.file_stat, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmts_.file_stat) == SQLITE_ROW) {
        int64_t db_file_id  = sqlite3_column_int64(stmts_.file_stat, 0);
        int64_t db_size     = sqlite3_column_int64(stmts_.file_stat, 1);
        int64_t db_mtime    = sqlite3_column_int64(stmts_.file_stat, 2);
        bool    db_binary   = sqlite3_column_int (stmts_.file_stat, 3) != 0;
        bool    stat_match  = (db_size == static_cast<int64_t>(fsize) &&
                               db_mtime == mtime);

        bool chunks_ok = true;
        if (stat_match && !db_binary && vectors_db_ &&
            config_.index.semantic_search_enabled && stmts_.file_has_chunks) {
            sqlite3_reset(stmts_.file_has_chunks);
            sqlite3_bind_int64(stmts_.file_has_chunks, 1, db_file_id);
            if (sqlite3_step(stmts_.file_has_chunks) == SQLITE_ROW) {
                chunks_ok = sqlite3_column_int(stmts_.file_has_chunks, 0) != 0;
            }
        }

        if (stat_match && chunks_ok) {
            ++stats_.files_total;
            if (db_binary) ++stats_.files_binary;
            else           ++stats_.files_indexed;
            return;
        }
    }

    std::string ext = rel_path.extension().string();
    std::string language = detect_language(ext);

    // Try extractor for this file type, fall back to raw content read
    std::string content;
    std::vector<ExtractedHeading> headings;
    bool binary = false;

    ITextExtractor* extractor = extractors_.find(ext);
    if (extractor) {
        // Check file size before invoking extractor
        if (config_.index.max_file_size_kb > 0 &&
            fsize > static_cast<uintmax_t>(config_.index.max_file_size_kb) * 1024) {
            log_fs()->trace("Skipping large file ({} KB): {}", fsize / 1024, abs_path.string());
            binary = true;
        } else {
            auto result = extractor->extract(abs_path);
            binary = result.is_binary;
            if (!binary) {
                content = std::move(result.text);
                headings = std::move(result.headings);
            }
        }
    } else {
        content = read_file_content(abs_path, config_.index.max_file_size_kb);
        binary = content.empty() ||
                 is_binary_content(content.data(), content.size());
    }

    // Count lines in the extracted/text content -- for text files this is the
    // number of '\n' (plus 1 if non-empty); for binary files line_count = 0.
    // For PDF/DOCX/XLSX `content` is the extractor's pseudo-line text, so the
    // count is "pseudo-lines" (per-page for PDFs). Surfaces via FileEntry +
    // get_file_line_count so get_file_outline / list_directory can show it.
    int64_t line_count = 0;
    if (!binary && !content.empty()) {
        line_count = 1;
        for (char c : content) if (c == '\n') ++line_count;
        // A trailing '\n' counts as a separator, not a fresh empty line.
        if (content.back() == '\n') --line_count;
    }

    // Upsert files row
    sqlite3_reset(stmts_.upsert_file);
    sqlite3_bind_text(stmts_.upsert_file, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmts_.upsert_file, 2, abs_path.string().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmts_.upsert_file, 3, static_cast<int64_t>(fsize));
    sqlite3_bind_int64(stmts_.upsert_file, 4, mtime);
    sqlite3_bind_text(stmts_.upsert_file, 5, ext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmts_.upsert_file, 6, binary ? 1 : 0);
    sqlite3_bind_text(stmts_.upsert_file, 7, language.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmts_.upsert_file, 8, unix_now());
    sqlite3_bind_int64(stmts_.upsert_file, 9, line_count);
    sqlite3_step(stmts_.upsert_file);

    ++stats_.files_total;

    if (binary) {
        ++stats_.files_binary;
        log_fs()->trace("Indexed (binary, skipped content): {}", rel_str);
        return;
    }

    // Get the file_id (just inserted/updated)
    sqlite3_reset(stmts_.file_id);
    sqlite3_bind_text(stmts_.file_id, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmts_.file_id);
    if (rc != SQLITE_ROW) return;
    int64_t file_id = sqlite3_column_int64(stmts_.file_id, 0);

    // Delete old FTS/symbols/headings/chunks for this file (idempotent re-index)
    delete_fts(rel_str);

    sqlite3_reset(stmts_.delete_syms);
    sqlite3_bind_int64(stmts_.delete_syms, 1, file_id);
    sqlite3_step(stmts_.delete_syms);

    sqlite3_reset(stmts_.delete_heads);
    sqlite3_bind_int64(stmts_.delete_heads, 1, file_id);
    sqlite3_step(stmts_.delete_heads);

    if (vectors_db_ && config_.index.semantic_search_enabled) {
        if (auto* del_vecs = ensure_delete_chunk_vecs_stmt(vectors_db_, stmts_)) {
            sqlite3_reset(del_vecs);
            sqlite3_bind_int64(del_vecs, 1, file_id);
            sqlite3_step(del_vecs);
        }

        sqlite3_reset(stmts_.delete_chunks);
        sqlite3_bind_int64(stmts_.delete_chunks, 1, file_id);
        sqlite3_step(stmts_.delete_chunks);
    }

    // Insert FTS content
    insert_fts(rel_str, content);
    ++stats_.files_indexed;

    // Insert headings (from extractor)
    if (!headings.empty()) {
        insert_headings(file_id, headings);
    }

    // Extract symbols (Tree-sitter)
    std::vector<SymbolSpan> symbol_spans;
    if (config_.index.code_parsing_enabled) {
        extract_symbols(file_id, content, language, symbol_spans);
    }

    // Create semantic chunks (if enabled)
    if (vectors_db_ && config_.index.semantic_search_enabled && !content.empty()) {
        std::vector<Chunk> chunks;
        if (!symbol_spans.empty()) {
            // S6.18 Task A.3 -- leaf-symbol pre-filter. The symbol extractor
            // emits both container symbols (namespace, class) and the leaves
            // they enclose (method, function). Feeding the full set to
            // chunk_code overlaps every leaf inside its container -- on the
            // TestLocalVibe2 corpus that produced 220/238 overlapping chunks
            // for a 3-file workspace and doubled the embedder's workload.
            // Keep only spans that don't strictly contain another span.
            // O(n^2) sweep is fine; n is <500 per file in practice.
            std::vector<SymbolSpan> leaf_spans;
            leaf_spans.reserve(symbol_spans.size());
            for (size_t i = 0; i < symbol_spans.size(); ++i) {
                const auto& s = symbol_spans[i];
                bool contains_another = false;
                for (size_t j = 0; j < symbol_spans.size(); ++j) {
                    if (i == j) continue;
                    const auto& other = symbol_spans[j];
                    if (s.line_start <= other.line_start
                        && s.line_end   >= other.line_end
                        && (s.line_start < other.line_start
                            || s.line_end   > other.line_end)) {
                        contains_another = true;
                        break;
                    }
                }
                if (!contains_another) leaf_spans.push_back(s);
            }
            chunks = chunk_code(content, leaf_spans,
                                config_.index.chunk_size_lines, config_.index.chunk_overlap_lines);
        } else if (!headings.empty()) {
            chunks = chunk_document(content, headings,
                                    config_.index.chunk_size_lines, config_.index.chunk_overlap_lines);
        } else {
            chunks = chunk_sliding_window(content,
                                          config_.index.chunk_size_lines, config_.index.chunk_overlap_lines);
        }

        std::vector<int64_t> chunk_ids;
        for (int ci = 0; ci < static_cast<int>(chunks.size()); ++ci) {
            const auto& chunk = chunks[ci];
            // S6.18 Task A.4 -- belt-and-suspenders skip for any chunk whose
            // content slipped through chunk_code's defensive walk as empty
            // (e.g. via a future codepath we haven't audited). Persisting
            // empty content wastes a chunks-row + an embedder forward pass.
            if (chunk.content.empty()) continue;
            sqlite3_reset(stmts_.insert_chunk);
            sqlite3_bind_int64(stmts_.insert_chunk, 1, file_id);
            sqlite3_bind_int(stmts_.insert_chunk, 2, ci);
            sqlite3_bind_int(stmts_.insert_chunk, 3, chunk.start_line);
            sqlite3_bind_int(stmts_.insert_chunk, 4, chunk.end_line);
            sqlite3_bind_text(stmts_.insert_chunk, 5, chunk.content.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmts_.insert_chunk);

            int64_t chunk_id = sqlite3_last_insert_rowid(vectors_db_->handle());
            chunk_ids.push_back(chunk_id);
        }

        stats_.chunks_total += static_cast<int>(chunk_ids.size());

        if (on_chunks_created && !chunk_ids.empty()) {
            on_chunks_created(std::move(chunk_ids));
        }
    }

    log_fs()->trace("Indexed: {} (lang={}, {} bytes)", rel_str, language, content.size());
}

void Indexer::remove_file(const fs::path& rel_path)
{
    std::string rel_str = rel_path.string();
    std::replace(rel_str.begin(), rel_str.end(), '\\', '/');

    // Get file_id before deleting
    sqlite3_reset(stmts_.file_id);
    sqlite3_bind_text(stmts_.file_id, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmts_.file_id);
    if (rc == SQLITE_ROW) {
        int64_t file_id = sqlite3_column_int64(stmts_.file_id, 0);

        sqlite3_reset(stmts_.delete_syms);
        sqlite3_bind_int64(stmts_.delete_syms, 1, file_id);
        sqlite3_step(stmts_.delete_syms);

        sqlite3_reset(stmts_.delete_heads);
        sqlite3_bind_int64(stmts_.delete_heads, 1, file_id);
        sqlite3_step(stmts_.delete_heads);

        // Clean up chunks and their vectors (in vectors.db)
        if (vectors_db_) {
            if (auto* del_vecs = ensure_delete_chunk_vecs_stmt(vectors_db_, stmts_)) {
                sqlite3_reset(del_vecs);
                sqlite3_bind_int64(del_vecs, 1, file_id);
                sqlite3_step(del_vecs);
            }

            sqlite3_reset(stmts_.delete_chunks);
            sqlite3_bind_int64(stmts_.delete_chunks, 1, file_id);
            sqlite3_step(stmts_.delete_chunks);
        }
    }

    delete_fts(rel_str);

    sqlite3_reset(stmts_.delete_file);
    sqlite3_bind_text(stmts_.delete_file, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmts_.delete_file);

    log_fs()->trace("Removed from index: {}", rel_str);
}

// -- Full traversal -----------------------------------------------------------

void Indexer::build_initial()
{
    spdlog::info("Starting initial index build for {}", root_.string());
    auto t0 = std::chrono::steady_clock::now();

    main_db_.exec("BEGIN TRANSACTION");
    if (vectors_db_) vectors_db_->exec("BEGIN TRANSACTION");

    for (auto& entry : fs::recursive_directory_iterator(
             root_, fs::directory_options::skip_permission_denied))
    {
        if (!entry.is_regular_file()) continue;

        fs::path rel = fs::relative(entry.path(), root_);
        if (is_excluded(rel)) continue;

        index_file(rel);
    }

    main_db_.exec("COMMIT");
    if (vectors_db_) vectors_db_->exec("COMMIT");

    // S4.L -- drop any rows left over from a prior session whose excludes
    // were narrower (e.g. user added a new `.gitignore` line between runs).
    // Cheap on-open reconciliation; rows for paths that still pass the
    // exclude filter survive untouched.
    int reconciled = reconcile_excluded_files();
    if (reconciled > 0) {
        spdlog::info("Dropped {} stale row(s) from index after exclude check",
                     reconciled);
    }

    // Also drop any row whose on-disk path is gone or has become a directory.
    // Older builds inserted directory rows when the watcher fired a "modified"
    // event for the dir itself; those rows persist in stale DBs and surface
    // as phantom file entries in the Files panel (e.g. a "src" file next to
    // the "src" folder). One stat() per row, runs once per workspace open.
    int nonfile = reconcile_nonexistent_files();
    if (nonfile > 0) {
        spdlog::info("Dropped {} stale row(s) from index for paths that "
                     "no longer resolve to a regular file", nonfile);
    }

    // Overwrite the per-session counters with authoritative DB totals -- after
    // the fast-path skip landed, symbols/headings/chunks_total only reflect
    // files re-indexed this session, which reads as "0 symbols" on a warm
    // start. Callers (main.cpp, integration harness) feed these into the
    // agent's WorkspaceMetadata, so they need the full on-disk totals.
    if (auto* s = main_db_.prepare("SELECT COUNT(*) FROM symbols"); s) {
        if (sqlite3_step(s) == SQLITE_ROW)
            stats_.symbols_total = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (auto* s = main_db_.prepare("SELECT COUNT(*) FROM headings"); s) {
        if (sqlite3_step(s) == SQLITE_ROW)
            stats_.headings_total = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (vectors_db_) {
        if (auto* s = vectors_db_->prepare("SELECT COUNT(*) FROM chunks"); s) {
            if (sqlite3_step(s) == SQLITE_ROW)
                stats_.chunks_total = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    spdlog::info("Initial index complete: {} files ({} text, {} binary), "
                 "{} symbols, {} headings in {} ms",
                 stats_.files_total, stats_.files_indexed, stats_.files_binary,
                 stats_.symbols_total, stats_.headings_total, elapsed.count());

    if (on_activity) {
        std::string sum = "Initial index: " + std::to_string(stats_.files_total) +
                          " files (" + std::to_string(stats_.files_indexed) + " text, " +
                          std::to_string(stats_.files_binary) + " binary) in " +
                          std::to_string(elapsed.count()) + " ms";
        std::string det = "symbols: " + std::to_string(stats_.symbols_total) +
                          "\nheadings: " + std::to_string(stats_.headings_total) +
                          "\nchunks: " + std::to_string(stats_.chunks_total);
        on_activity(sum, det);
    }
}

// -- Incremental updates ------------------------------------------------------

void Indexer::process_events(const std::vector<FileEvent>& events)
{
    if (events.empty()) return;

    std::lock_guard<std::mutex> serial(process_mutex_);

    // S4.L -- if any event touches a `.gitignore`, reload the pattern set
    // BEFORE we filter the rest of the batch. Then re-index files that newly
    // pass the filter (a removed pattern may unblock a previously-excluded
    // file that's still in this batch). Reload runs once per batch even if
    // multiple .gitignore files changed.
    bool gitignore_changed = false;
    if (config_.index.respect_gitignore) {
        for (auto& ev : events) {
            if (ev.path.filename() == ".gitignore"
                || (ev.action == FileAction::Moved
                    && ev.old_path.filename() == ".gitignore")) {
                gitignore_changed = true;
                break;
            }
        }
    }
    int gitignore_dropped = 0;
    if (gitignore_changed) {
        gitignore_dropped = reload_gitignore();
        if (on_activity && gitignore_dropped > 0) {
            on_activity("Excluded " + std::to_string(gitignore_dropped) +
                            " path(s) from index after .gitignore update",
                        "");
        } else if (on_activity) {
            on_activity(".gitignore reloaded ("
                        + std::to_string(gitignore_patterns_.size())
                        + " pattern(s))",
                        "");
        }
    }

    // Filter out events that won't change the index. The OS file watcher
    // (efsw) fires a separate "modified" event for the parent directory
    // every time a child is added/removed; without this filter the activity
    // log shows phantom rows like "~ src" alongside the real "+ src/main.js"
    // and the header miscounts ("2 file events" for one actual file). For
    // Added/Modified/Moved we filter by `fs::is_regular_file` -- the
    // dispositive test we already use inside `index_file`. For Deleted we
    // keep the event (the path no longer exists, so we can't probe it; if
    // the row wasn't in the DB, `remove_file` is a cheap no-op anyway --
    // the user-visible cost is just that a deleted directory might show
    // up here, which is rare in our flows).
    std::vector<FileEvent> filtered;
    filtered.reserve(events.size());
    for (auto& ev : events) {
        if (is_excluded(ev.path))
            continue;
        if (ev.action == FileAction::Deleted) {
            filtered.push_back(ev);
            continue;
        }
        std::error_code ec;
        fs::path abs_path = root_ / ev.path;
        if (fs::is_regular_file(abs_path, ec))
            filtered.push_back(ev);
    }

    if (filtered.empty()) {
        log_fs()->trace("Processed 0 file events ({} dropped as non-files)",
                      events.size());
        return;
    }

    main_db_.exec("BEGIN TRANSACTION");
    if (vectors_db_) vectors_db_->exec("BEGIN TRANSACTION");

    const int total = static_cast<int>(filtered.size());
    int done = 0;
    if (on_progress) on_progress(0, total);

    for (auto& ev : filtered) {
        switch (ev.action) {
            case FileAction::Added:
            case FileAction::Modified:
                index_file(ev.path);
                break;
            case FileAction::Deleted:
                remove_file(ev.path);
                break;
            case FileAction::Moved:
                remove_file(ev.old_path);
                index_file(ev.path);
                break;
        }
        ++done;
        if (on_progress) on_progress(done, total);
    }

    main_db_.exec("COMMIT");
    if (vectors_db_) vectors_db_->exec("COMMIT");

    log_fs()->trace("Processed {} file events ({} raw, {} dropped as non-files)",
                  filtered.size(), events.size(),
                  events.size() - filtered.size());

    if (on_activity) {
        std::string sum = "Indexed " + std::to_string(filtered.size()) + " file" +
                          (filtered.size() == 1 ? "" : "s");
        std::string det;
        for (auto& ev : filtered) {
            det += (ev.action == FileAction::Deleted ? "- " :
                    ev.action == FileAction::Added   ? "+ " :
                    ev.action == FileAction::Moved   ? "> " : "~ ");
            det += ev.path.string();
            if (ev.action == FileAction::Moved)
                det += " (from " + ev.old_path.string() + ")";
            det += "\n";
        }
        on_activity(sum, det);
    }
}

} // namespace locus

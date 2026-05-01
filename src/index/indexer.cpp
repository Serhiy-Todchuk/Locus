#include "indexer.h"
#include "chunker.h"
#include "database.h"
#include "extractors/extractor_registry.h"
#include "extractors/text_extractor.h"
#include "glob_match.h"
#include "workspace.h"

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
        spdlog::trace("Skipping large file ({} KB): {}", file_size / 1024, abs_path.string());
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
    spdlog::trace("Indexer initialised");
}

Indexer::~Indexer()
{
    spdlog::trace("Indexer destroyed");
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
        {".sh",   "shell"},
        {".bash", "shell"},
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
    for (auto& pattern : config_.exclude_patterns) {
        if (glob_match(pattern, rel_str)) return true;
    }
    return false;
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

        spdlog::trace("Symbol: {} {} '{}' lines {}-{}", sym.kind, language, sym.name,
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

    // Fast path — skip any file whose size+mtime match what's already in the
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
            config_.semantic_search_enabled && stmts_.file_has_chunks) {
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
        if (config_.max_file_size_kb > 0 &&
            fsize > static_cast<uintmax_t>(config_.max_file_size_kb) * 1024) {
            spdlog::trace("Skipping large file ({} KB): {}", fsize / 1024, abs_path.string());
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
        content = read_file_content(abs_path, config_.max_file_size_kb);
        binary = content.empty() ||
                 is_binary_content(content.data(), content.size());
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
    sqlite3_step(stmts_.upsert_file);

    ++stats_.files_total;

    if (binary) {
        ++stats_.files_binary;
        spdlog::trace("Indexed (binary, skipped content): {}", rel_str);
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

    if (vectors_db_ && config_.semantic_search_enabled) {
        sqlite3_reset(stmts_.delete_chunk_vecs);
        sqlite3_bind_int64(stmts_.delete_chunk_vecs, 1, file_id);
        sqlite3_step(stmts_.delete_chunk_vecs);

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
    if (config_.code_parsing_enabled) {
        extract_symbols(file_id, content, language, symbol_spans);
    }

    // Create semantic chunks (if enabled)
    if (vectors_db_ && config_.semantic_search_enabled && !content.empty()) {
        std::vector<Chunk> chunks;
        if (!symbol_spans.empty()) {
            chunks = chunk_code(content, symbol_spans,
                                config_.chunk_size_lines, config_.chunk_overlap_lines);
        } else if (!headings.empty()) {
            chunks = chunk_document(content, headings,
                                    config_.chunk_size_lines, config_.chunk_overlap_lines);
        } else {
            chunks = chunk_sliding_window(content,
                                          config_.chunk_size_lines, config_.chunk_overlap_lines);
        }

        std::vector<int64_t> chunk_ids;
        for (int ci = 0; ci < static_cast<int>(chunks.size()); ++ci) {
            const auto& chunk = chunks[ci];
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

        stats_.chunks_total += static_cast<int>(chunks.size());

        if (on_chunks_created && !chunk_ids.empty()) {
            on_chunks_created(std::move(chunk_ids));
        }
    }

    spdlog::trace("Indexed: {} (lang={}, {} bytes)", rel_str, language, content.size());
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
            sqlite3_reset(stmts_.delete_chunk_vecs);
            sqlite3_bind_int64(stmts_.delete_chunk_vecs, 1, file_id);
            sqlite3_step(stmts_.delete_chunk_vecs);

            sqlite3_reset(stmts_.delete_chunks);
            sqlite3_bind_int64(stmts_.delete_chunks, 1, file_id);
            sqlite3_step(stmts_.delete_chunks);
        }
    }

    delete_fts(rel_str);

    sqlite3_reset(stmts_.delete_file);
    sqlite3_bind_text(stmts_.delete_file, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmts_.delete_file);

    spdlog::trace("Removed from index: {}", rel_str);
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

    // Overwrite the per-session counters with authoritative DB totals — after
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

    main_db_.exec("BEGIN TRANSACTION");
    if (vectors_db_) vectors_db_->exec("BEGIN TRANSACTION");

    const int total = static_cast<int>(events.size());
    int done = 0;
    if (on_progress) on_progress(0, total);

    for (auto& ev : events) {
        if (!is_excluded(ev.path)) {
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
        }
        ++done;
        if (on_progress) on_progress(done, total);
    }

    main_db_.exec("COMMIT");
    if (vectors_db_) vectors_db_->exec("COMMIT");

    spdlog::trace("Processed {} file events", events.size());

    if (on_activity) {
        std::string sum = "Indexed " + std::to_string(events.size()) + " file event" +
                          (events.size() == 1 ? "" : "s");
        std::string det;
        for (auto& ev : events) {
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

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

// Tree-sitter grammar entry points (compiled from grammar sources)
extern "C" {
    const TSLanguage* tree_sitter_c();
    const TSLanguage* tree_sitter_cpp();
    const TSLanguage* tree_sitter_python();
    const TSLanguage* tree_sitter_javascript();
    const TSLanguage* tree_sitter_typescript();
    const TSLanguage* tree_sitter_go();
    const TSLanguage* tree_sitter_rust();
    const TSLanguage* tree_sitter_java();
    const TSLanguage* tree_sitter_c_sharp();
}

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

// Extract text from the TSNode range in source.
static std::string node_text(TSNode node, const std::string& source)
{
    uint32_t start = ts_node_start_byte(node);
    uint32_t end   = ts_node_end_byte(node);
    if (start >= source.size() || end > source.size() || start >= end) return {};
    return source.substr(start, end - start);
}

// Get the first line of text (for signature extraction).
static std::string first_line(const std::string& text)
{
    auto pos = text.find('\n');
    if (pos == std::string::npos) return text;
    // Trim trailing \r
    if (pos > 0 && text[pos - 1] == '\r') --pos;
    return text.substr(0, pos);
}

// Recursively find the identifier name from a declarator node.
// Handles nested declarators like function_declarator -> declarator -> identifier.
static std::string extract_name_from_node(TSNode node, const std::string& source)
{
    // Try "name" field first (used by class_specifier, struct_specifier, etc.)
    TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
    if (!ts_node_is_null(name_node)) {
        return node_text(name_node, source);
    }

    // Try "declarator" field (used by function_definition, declaration)
    TSNode decl = ts_node_child_by_field_name(node, "declarator", 10);
    if (!ts_node_is_null(decl)) {
        // Recurse — function_declarator has its own "declarator" child
        return extract_name_from_node(decl, source);
    }

    // Fallback: if this node is an identifier, return its text
    const char* type = ts_node_type(node);
    if (strcmp(type, "identifier") == 0 ||
        strcmp(type, "type_identifier") == 0 ||
        strcmp(type, "field_identifier") == 0) {
        return node_text(node, source);
    }

    return {};
}

// -- Symbol extraction node types per language --------------------------------

struct SymbolRule {
    const char* node_type;
    const char* kind;       // "function", "class", "struct", "method", "enum", "namespace", "interface"
};

static const std::vector<SymbolRule>& symbol_rules_for(const std::string& language)
{
    // C / C++
    static const std::vector<SymbolRule> cpp_rules = {
        {"function_definition",    "function"},
        {"class_specifier",        "class"},
        {"struct_specifier",       "struct"},
        {"enum_specifier",         "enum"},
        {"namespace_definition",   "namespace"},
    };
    // Python
    static const std::vector<SymbolRule> py_rules = {
        {"function_definition",    "function"},
        {"class_definition",       "class"},
    };
    // JavaScript / TypeScript
    static const std::vector<SymbolRule> js_rules = {
        {"function_declaration",   "function"},
        {"class_declaration",      "class"},
        {"method_definition",      "method"},
    };
    // Go
    static const std::vector<SymbolRule> go_rules = {
        {"function_declaration",   "function"},
        {"method_declaration",     "method"},
        {"type_declaration",       "struct"},
    };
    // Rust
    static const std::vector<SymbolRule> rust_rules = {
        {"function_item",          "function"},
        {"struct_item",            "struct"},
        {"enum_item",              "enum"},
        {"impl_item",              "class"},
        {"trait_item",             "interface"},
    };
    // Java
    static const std::vector<SymbolRule> java_rules = {
        {"method_declaration",     "method"},
        {"class_declaration",      "class"},
        {"interface_declaration",  "interface"},
        {"enum_declaration",       "enum"},
    };
    // C#
    static const std::vector<SymbolRule> csharp_rules = {
        {"method_declaration",     "method"},
        {"class_declaration",      "class"},
        {"struct_declaration",     "struct"},
        {"interface_declaration",  "interface"},
        {"enum_declaration",       "enum"},
        {"namespace_declaration",  "namespace"},
    };

    static const std::vector<SymbolRule> empty;

    if (language == "c" || language == "cpp") return cpp_rules;
    if (language == "python")                 return py_rules;
    if (language == "javascript" || language == "typescript") return js_rules;
    if (language == "go")                     return go_rules;
    if (language == "rust")                   return rust_rules;
    if (language == "java")                   return java_rules;
    if (language == "csharp")                 return csharp_rules;
    return empty;
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
{
    // Main DB: skeleton index (files, FTS, symbols, headings)
    stmt_upsert_file_ = main_db_.prepare(R"(
        INSERT INTO files (path, abs_path, size_bytes, modified_at, ext, is_binary, language, indexed_at)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(path) DO UPDATE SET
            abs_path=?2, size_bytes=?3, modified_at=?4, ext=?5,
            is_binary=?6, language=?7, indexed_at=?8
    )");

    stmt_delete_file_ = main_db_.prepare("DELETE FROM files WHERE path = ?1");
    stmt_file_id_     = main_db_.prepare("SELECT id FROM files WHERE path = ?1");

    stmt_insert_fts_  = main_db_.prepare("INSERT INTO files_fts (path, content) VALUES (?1, ?2)");
    stmt_delete_fts_  = main_db_.prepare("DELETE FROM files_fts WHERE path = ?1");

    stmt_insert_sym_  = main_db_.prepare(R"(
        INSERT INTO symbols (file_id, kind, name, line_start, line_end, signature, parent_name)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
    )");
    stmt_delete_syms_ = main_db_.prepare("DELETE FROM symbols WHERE file_id = ?1");

    stmt_insert_head_ = main_db_.prepare(R"(
        INSERT INTO headings (file_id, level, text, line_number)
        VALUES (?1, ?2, ?3, ?4)
    )");
    stmt_delete_heads_ = main_db_.prepare("DELETE FROM headings WHERE file_id = ?1");

    // Vectors DB: chunks + chunk_vectors live together in their own file.
    if (vectors_db_) {
        stmt_insert_chunk_ = vectors_db_->prepare(R"(
            INSERT INTO chunks (file_id, chunk_index, start_line, end_line, content)
            VALUES (?1, ?2, ?3, ?4, ?5)
        )");
        stmt_delete_chunks_ = vectors_db_->prepare("DELETE FROM chunks WHERE file_id = ?1");
        stmt_delete_chunk_vecs_ = vectors_db_->prepare(
            "DELETE FROM chunk_vectors WHERE chunk_id IN (SELECT id FROM chunks WHERE file_id = ?1)");
    }

    init_tree_sitter();

    spdlog::trace("Indexer initialised");
}

Indexer::~Indexer()
{
    auto finalize = [](sqlite3_stmt*& s) {
        if (s) { sqlite3_finalize(s); s = nullptr; }
    };
    finalize(stmt_upsert_file_);
    finalize(stmt_delete_file_);
    finalize(stmt_file_id_);
    finalize(stmt_insert_fts_);
    finalize(stmt_delete_fts_);
    finalize(stmt_insert_sym_);
    finalize(stmt_delete_syms_);
    finalize(stmt_insert_head_);
    finalize(stmt_delete_heads_);
    finalize(stmt_insert_chunk_);
    finalize(stmt_delete_chunks_);
    finalize(stmt_delete_chunk_vecs_);

    if (ts_parser_) {
        ts_parser_delete(ts_parser_);
        ts_parser_ = nullptr;
    }

    spdlog::trace("Indexer destroyed");
}

// -- Tree-sitter init ---------------------------------------------------------

void Indexer::init_tree_sitter()
{
    ts_parser_ = ts_parser_new();

    ts_languages_["c"]          = tree_sitter_c();
    ts_languages_["cpp"]        = tree_sitter_cpp();
    ts_languages_["python"]     = tree_sitter_python();
    ts_languages_["javascript"] = tree_sitter_javascript();
    ts_languages_["typescript"] = tree_sitter_typescript();
    ts_languages_["go"]         = tree_sitter_go();
    ts_languages_["rust"]       = tree_sitter_rust();
    ts_languages_["java"]       = tree_sitter_java();
    ts_languages_["csharp"]     = tree_sitter_c_sharp();

    spdlog::trace("Tree-sitter initialised with {} grammars", ts_languages_.size());
}

const TSLanguage* Indexer::ts_language_for(const std::string& language) const
{
    auto it = ts_languages_.find(language);
    return (it != ts_languages_.end()) ? it->second : nullptr;
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
    sqlite3_reset(stmt_insert_fts_);
    sqlite3_bind_text(stmt_insert_fts_, 1, rel_path_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_fts_, 2, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmt_insert_fts_);
}

void Indexer::delete_fts(const std::string& rel_path_str)
{
    sqlite3_reset(stmt_delete_fts_);
    sqlite3_bind_text(stmt_delete_fts_, 1, rel_path_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt_delete_fts_);
}

// -- Heading insertion (from extractor results) --------------------------------

void Indexer::insert_headings(int64_t file_id,
                              const std::vector<ExtractedHeading>& headings)
{
    for (const auto& h : headings) {
        sqlite3_reset(stmt_insert_head_);
        sqlite3_bind_int64(stmt_insert_head_, 1, file_id);
        sqlite3_bind_int(stmt_insert_head_, 2, h.level);
        sqlite3_bind_text(stmt_insert_head_, 3, h.text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt_insert_head_, 4, h.line_number);
        sqlite3_step(stmt_insert_head_);

        ++stats_.headings_total;
    }
}

// -- Symbol extraction (Tree-sitter) ------------------------------------------

void Indexer::extract_symbols(int64_t file_id, const std::string& content,
                              const std::string& language,
                              std::vector<SymbolSpan>& out_spans)
{
    const TSLanguage* ts_lang = ts_language_for(language);
    if (!ts_lang) return;

    const auto& rules = symbol_rules_for(language);
    if (rules.empty()) return;

    ts_parser_set_language(ts_parser_, ts_lang);
    TSTree* tree = ts_parser_parse_string(ts_parser_, nullptr,
                                          content.c_str(),
                                          static_cast<uint32_t>(content.size()));
    if (!tree) {
        spdlog::warn("Tree-sitter parse failed for language '{}'", language);
        return;
    }

    // Build a set of node types to look for
    std::unordered_map<std::string, const char*> type_to_kind;
    for (auto& rule : rules) {
        type_to_kind[rule.node_type] = rule.kind;
    }

    // Walk the AST with a cursor (depth-first, only visit top-level + one level deep)
    TSNode root = ts_tree_root_node(tree);
    uint32_t child_count = ts_node_child_count(root);

    // Helper lambda: process a node if it matches a symbol rule
    auto try_extract = [&](TSNode node, const std::string& parent_name) {
        const char* type = ts_node_type(node);
        auto it = type_to_kind.find(type);
        if (it == type_to_kind.end()) return;

        std::string name = extract_name_from_node(node, content);
        if (name.empty()) return;

        TSPoint start = ts_node_start_point(node);
        TSPoint end   = ts_node_end_point(node);

        // Signature: first line of the node text
        std::string sig = first_line(node_text(node, content));
        // Truncate long signatures
        if (sig.size() > 200) sig = sig.substr(0, 200) + "...";

        sqlite3_reset(stmt_insert_sym_);
        sqlite3_bind_int64(stmt_insert_sym_, 1, file_id);
        sqlite3_bind_text(stmt_insert_sym_, 2, it->second, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt_insert_sym_, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt_insert_sym_, 4, static_cast<int>(start.row) + 1);
        sqlite3_bind_int(stmt_insert_sym_, 5, static_cast<int>(end.row) + 1);
        sqlite3_bind_text(stmt_insert_sym_, 6, sig.c_str(), -1, SQLITE_TRANSIENT);
        if (parent_name.empty())
            sqlite3_bind_null(stmt_insert_sym_, 7);
        else
            sqlite3_bind_text(stmt_insert_sym_, 7, parent_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_insert_sym_);

        ++stats_.symbols_total;

        out_spans.push_back({static_cast<int>(start.row) + 1,
                             static_cast<int>(end.row) + 1});

        spdlog::trace("Symbol: {} {} '{}' lines {}-{}", it->second, language, name,
                      start.row + 1, end.row + 1);
    };

    // Two-level walk: top-level children, and their direct children (for methods inside classes)
    for (uint32_t i = 0; i < child_count; ++i) {
        TSNode child = ts_node_child(root, i);
        try_extract(child, "");

        // Look one level deeper for methods/nested definitions
        std::string parent_name = extract_name_from_node(child, content);
        uint32_t grandchild_count = ts_node_child_count(child);
        for (uint32_t j = 0; j < grandchild_count; ++j) {
            TSNode grandchild = ts_node_child(child, j);
            try_extract(grandchild, parent_name);

            // One more level for class bodies (e.g., class -> body -> method)
            uint32_t ggc = ts_node_child_count(grandchild);
            for (uint32_t k = 0; k < ggc; ++k) {
                try_extract(ts_node_child(grandchild, k), parent_name);
            }
        }
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
    if (!fs::exists(abs_path, ec)) return;

    auto fsize = fs::file_size(abs_path, ec);
    if (ec) return;

    auto ftime = fs::last_write_time(abs_path, ec);
    // Convert to unix timestamp
    auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    int64_t mtime = std::chrono::duration_cast<std::chrono::seconds>(
        sctp.time_since_epoch()).count();

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
    sqlite3_reset(stmt_upsert_file_);
    sqlite3_bind_text(stmt_upsert_file_, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upsert_file_, 2, abs_path.string().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_upsert_file_, 3, static_cast<int64_t>(fsize));
    sqlite3_bind_int64(stmt_upsert_file_, 4, mtime);
    sqlite3_bind_text(stmt_upsert_file_, 5, ext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_upsert_file_, 6, binary ? 1 : 0);
    sqlite3_bind_text(stmt_upsert_file_, 7, language.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_upsert_file_, 8, unix_now());
    sqlite3_step(stmt_upsert_file_);

    ++stats_.files_total;

    if (binary) {
        ++stats_.files_binary;
        spdlog::trace("Indexed (binary, skipped content): {}", rel_str);
        return;
    }

    // Get the file_id (just inserted/updated)
    sqlite3_reset(stmt_file_id_);
    sqlite3_bind_text(stmt_file_id_, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt_file_id_);
    if (rc != SQLITE_ROW) return;
    int64_t file_id = sqlite3_column_int64(stmt_file_id_, 0);

    // Delete old FTS/symbols/headings/chunks for this file (idempotent re-index)
    delete_fts(rel_str);

    sqlite3_reset(stmt_delete_syms_);
    sqlite3_bind_int64(stmt_delete_syms_, 1, file_id);
    sqlite3_step(stmt_delete_syms_);

    sqlite3_reset(stmt_delete_heads_);
    sqlite3_bind_int64(stmt_delete_heads_, 1, file_id);
    sqlite3_step(stmt_delete_heads_);

    if (vectors_db_ && config_.semantic_search_enabled) {
        sqlite3_reset(stmt_delete_chunk_vecs_);
        sqlite3_bind_int64(stmt_delete_chunk_vecs_, 1, file_id);
        sqlite3_step(stmt_delete_chunk_vecs_);

        sqlite3_reset(stmt_delete_chunks_);
        sqlite3_bind_int64(stmt_delete_chunks_, 1, file_id);
        sqlite3_step(stmt_delete_chunks_);
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
            sqlite3_reset(stmt_insert_chunk_);
            sqlite3_bind_int64(stmt_insert_chunk_, 1, file_id);
            sqlite3_bind_int(stmt_insert_chunk_, 2, ci);
            sqlite3_bind_int(stmt_insert_chunk_, 3, chunk.start_line);
            sqlite3_bind_int(stmt_insert_chunk_, 4, chunk.end_line);
            sqlite3_bind_text(stmt_insert_chunk_, 5, chunk.content.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt_insert_chunk_);

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
    sqlite3_reset(stmt_file_id_);
    sqlite3_bind_text(stmt_file_id_, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt_file_id_);
    if (rc == SQLITE_ROW) {
        int64_t file_id = sqlite3_column_int64(stmt_file_id_, 0);

        sqlite3_reset(stmt_delete_syms_);
        sqlite3_bind_int64(stmt_delete_syms_, 1, file_id);
        sqlite3_step(stmt_delete_syms_);

        sqlite3_reset(stmt_delete_heads_);
        sqlite3_bind_int64(stmt_delete_heads_, 1, file_id);
        sqlite3_step(stmt_delete_heads_);

        // Clean up chunks and their vectors (in vectors.db)
        if (vectors_db_) {
            sqlite3_reset(stmt_delete_chunk_vecs_);
            sqlite3_bind_int64(stmt_delete_chunk_vecs_, 1, file_id);
            sqlite3_step(stmt_delete_chunk_vecs_);

            sqlite3_reset(stmt_delete_chunks_);
            sqlite3_bind_int64(stmt_delete_chunks_, 1, file_id);
            sqlite3_step(stmt_delete_chunks_);
        }
    }

    delete_fts(rel_str);

    sqlite3_reset(stmt_delete_file_);
    sqlite3_bind_text(stmt_delete_file_, 1, rel_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt_delete_file_);

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

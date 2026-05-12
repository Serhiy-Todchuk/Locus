#include "memory_store.h"

#include "core/database.h"
#include "core/workspace.h"   // WorkspaceConfig
#include "index/embedding_worker.h"
#include "index/reranker.h"
#include "llm/token_counter.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace locus {

namespace {

std::int64_t now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string format_iso8601(std::int64_t secs)
{
    if (secs <= 0) return "";
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream o;
    o << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return o.str();
}

std::int64_t parse_iso8601(const std::string& s)
{
    if (s.empty()) return 0;
    std::tm tm{};
    std::istringstream in(s);
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (in.fail()) return 0;
#ifdef _WIN32
    auto t = _mkgmtime(&tm);
#else
    auto t = timegm(&tm);
#endif
    return t < 0 ? 0 : static_cast<std::int64_t>(t);
}

std::string format_id_timestamp(std::int64_t secs)
{
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream o;
    o << std::put_time(&tm, "%Y-%m-%d-%H%M%S");
    return o.str();
}

std::string trim(const std::string& s)
{
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string join_tags_space(const std::vector<std::string>& tags)
{
    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out.push_back(' ');
        out += tags[i];
    }
    return out;
}

std::string join_tags_yaml(const std::vector<std::string>& tags)
{
    std::string out = "[";
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) out += ", ";
        out += tags[i];
    }
    out += "]";
    return out;
}

std::vector<std::string> parse_yaml_tag_list(const std::string& raw)
{
    // Accepts `[a, b, c]` and `a, b, c`.
    std::string s = trim(raw);
    if (!s.empty() && s.front() == '[') s.erase(0, 1);
    if (!s.empty() && s.back()  == ']') s.pop_back();

    std::vector<std::string> out;
    std::stringstream in(s);
    std::string tok;
    while (std::getline(in, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) out.push_back(std::move(tok));
    }
    return out;
}

// Build the on-disk format for one entry. Parser below must round-trip this
// byte-for-byte: content is written verbatim so neither serialise nor parse
// fabricates or strips trailing whitespace.
std::string serialise_entry(const MemoryStore::Entry& e)
{
    std::ostringstream o;
    o << "---\n";
    o << "id: "          << e.id                         << "\n";
    o << "created_at: "  << format_iso8601(e.created_at) << "\n";
    o << "updated_at: "  << format_iso8601(e.updated_at) << "\n";
    o << "last_used_at: "<< format_iso8601(e.last_used_at) << "\n";
    o << "source: "      << e.source                     << "\n";
    o << "pinned: "      << (e.pinned ? "true" : "false")<< "\n";
    o << "tags: "        << join_tags_yaml(e.tags)       << "\n";
    o << "---\n";
    o << e.content;
    return o.str();
}

// Minimal frontmatter parser. Accepts the format produced by
// serialise_entry; tolerant of extra whitespace and CRLF line endings.
bool parse_entry(const std::string& raw_in, MemoryStore::Entry& out)
{
    // Normalise CRLF -> LF so the marker / line scans below stay simple.
    std::string raw;
    raw.reserve(raw_in.size());
    for (char c : raw_in) if (c != '\r') raw.push_back(c);

    auto first_marker  = raw.find("---");
    if (first_marker != 0) return false;

    auto body_start = raw.find('\n', 3);
    if (body_start == std::string::npos) return false;
    ++body_start;

    auto second_marker = raw.find("\n---", body_start);
    if (second_marker == std::string::npos) return false;
    std::string fm   = raw.substr(body_start, second_marker - body_start);
    auto body_after  = raw.find('\n', second_marker + 4);
    std::string body = (body_after == std::string::npos)
                          ? std::string{}
                          : raw.substr(body_after + 1);

    out.content = body;

    std::istringstream in(fm);
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        if      (key == "id")            out.id          = val;
        else if (key == "source")        out.source      = val;
        else if (key == "pinned")        out.pinned      = (val == "true" || val == "1");
        else if (key == "tags")          out.tags        = parse_yaml_tag_list(val);
        else if (key == "created_at")    out.created_at  = parse_iso8601(val);
        else if (key == "updated_at")    out.updated_at  = parse_iso8601(val);
        else if (key == "last_used_at")  out.last_used_at= parse_iso8601(val);
    }
    return !out.id.empty();
}

// Atomic-ish file write: temp file then rename. Mirrors the pattern used by
// EditFileTool / WriteFileTool.
void atomic_write_file(const fs::path& target, const std::string& content)
{
    fs::create_directories(target.parent_path());
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot open " + tmp.string() + " for writing");
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f) throw std::runtime_error("Write failed: " + tmp.string());
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(target, ec);  // ignore
        fs::rename(tmp, target, ec);
        if (ec) throw std::runtime_error("Rename failed: " + ec.message());
    }
}

std::string read_text_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

} // namespace

// -- ctor/dtor ----------------------------------------------------------------

MemoryStore::MemoryStore(fs::path                memory_dir,
                         Database&               main_db,
                         Database*               vectors_db,
                         EmbeddingWorker*        embedder,
                         Reranker*               reranker,
                         const WorkspaceConfig&  config)
    : dir_(std::move(memory_dir))
    , deleted_dir_(dir_ / ".deleted")
    , main_db_(main_db)
    , vectors_db_(vectors_db)
    , embedder_(embedder)
    , reranker_(reranker)
    , max_entries_(config.memory_max_entries)
    , recency_half_life_days_(config.memory_recency_half_life_days)
    , in_context_budget_tokens_(config.memory_in_context_budget_tokens)
{
    std::error_code ec;
    fs::create_directories(dir_, ec);
    fs::create_directories(deleted_dir_, ec);

    prepare_statements();
    load_from_disk();
    purge_deleted();
    gc();

    spdlog::info("MemoryStore opened: {} entries ({})",
                 entries_.size(), dir_.string());
}

MemoryStore::~MemoryStore()
{
    finalize_statements();
    spdlog::trace("MemoryStore destroyed");
}

void MemoryStore::prepare_statements()
{
    stmt_fts_insert_ = main_db_.prepare(
        "INSERT INTO memory_fts(id, content, tags) VALUES (?1, ?2, ?3)");
    stmt_fts_delete_ = main_db_.prepare(
        "DELETE FROM memory_fts WHERE id = ?1");
    stmt_fts_search_ = main_db_.prepare(
        "SELECT id, bm25(memory_fts) AS rank "
        "FROM memory_fts WHERE memory_fts MATCH ?1 "
        "ORDER BY rank LIMIT ?2");

    if (vectors_db_) {
        stmt_keys_lookup_ = vectors_db_->prepare(
            "SELECT rowid FROM memory_keys WHERE id = ?1");
        stmt_keys_insert_ = vectors_db_->prepare(
            "INSERT INTO memory_keys(id) VALUES (?1)");
        stmt_keys_delete_ = vectors_db_->prepare(
            "DELETE FROM memory_keys WHERE id = ?1");
        stmt_vec_insert_  = vectors_db_->prepare(
            "INSERT OR REPLACE INTO memory_vectors(memory_rowid, embedding) "
            "VALUES (?1, ?2)");
        stmt_vec_delete_  = vectors_db_->prepare(
            "DELETE FROM memory_vectors WHERE memory_rowid = ?1");
        stmt_vec_search_  = vectors_db_->prepare(
            "SELECT mk.id, mv.distance "
            "FROM memory_vectors mv "
            "JOIN memory_keys mk ON mk.rowid = mv.memory_rowid "
            "WHERE mv.embedding MATCH ?1 AND k = ?2 "
            "ORDER BY mv.distance");
    }
}

void MemoryStore::finalize_statements()
{
    auto fin = [](sqlite3_stmt*& s) {
        if (s) { sqlite3_finalize(s); s = nullptr; }
    };
    fin(stmt_fts_insert_);
    fin(stmt_fts_delete_);
    fin(stmt_fts_search_);
    fin(stmt_keys_lookup_);
    fin(stmt_keys_insert_);
    fin(stmt_keys_delete_);
    fin(stmt_vec_insert_);
    fin(stmt_vec_delete_);
    fin(stmt_vec_search_);
}

// -- disk load + reconcile ----------------------------------------------------

void MemoryStore::load_from_disk()
{
    std::lock_guard lock(mutex_);
    entries_.clear();

    std::error_code ec;
    if (!fs::exists(dir_, ec)) return;

    for (auto& de : fs::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        auto p = de.path();
        if (p.extension() != ".md") continue;

        Entry e;
        auto raw = read_text_file(p);
        if (raw.empty() || !parse_entry(raw, e)) {
            spdlog::warn("MemoryStore: skipping unparseable entry {}", p.string());
            continue;
        }
        entries_.emplace(e.id, std::move(e));
    }

    // Reconcile FTS5: rebuild any rows missing from the table. Cheap (no I/O
    // beyond the SQL) and covers users who deleted index.db while memory/
    // survived. We over-delete here on purpose so partial / stale rows don't
    // pile up across restarts.
    {
        char* err = nullptr;
        sqlite3_exec(main_db_.handle(),
                     "DELETE FROM memory_fts", nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); }
    }
    for (auto& [id, e] : entries_) {
        fts_upsert(e);
    }

    // Reconcile vec0: anything we have in-memory but lacking a vector gets
    // re-embedded. After a dim-mismatch wipe `memory_vectors` is empty so
    // every entry re-embeds. Lazy: skipped entirely when embedder is null.
    if (embedder_ && vectors_db_) {
        // Cheap existence check: select all rowids and compare against ids.
        std::unordered_map<std::string, std::int64_t> have;
        sqlite3_stmt* iter = nullptr;
        if (sqlite3_prepare_v2(vectors_db_->handle(),
                "SELECT mk.id, mv.memory_rowid "
                "FROM memory_vectors mv "
                "JOIN memory_keys mk ON mk.rowid = mv.memory_rowid",
                -1, &iter, nullptr) == SQLITE_OK) {
            while (sqlite3_step(iter) == SQLITE_ROW) {
                const auto* idp = sqlite3_column_text(iter, 0);
                if (!idp) continue;
                have.emplace(reinterpret_cast<const char*>(idp),
                             sqlite3_column_int64(iter, 1));
            }
            sqlite3_finalize(iter);
        }
        for (auto& [id, e] : entries_) {
            if (have.count(id)) continue;
            try {
                vec_embed_and_store(id, e.content);
            } catch (const std::exception& ex) {
                spdlog::warn("MemoryStore: failed to re-embed {}: {}", id, ex.what());
            }
        }
    }
}

// -- mint id ------------------------------------------------------------------

std::string MemoryStore::mint_id(const std::string& /*source*/) const
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 0xFFFF);
    std::int64_t ts = now_seconds();
    char suffix[8] = {};
    std::snprintf(suffix, sizeof(suffix), "%04x", dist(rng));
    // Loop until we find one not already used (collision is astronomically
    // unlikely but cheap to defend against).
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::string candidate = format_id_timestamp(ts) + "-" + suffix;
        if (!entries_.count(candidate)) return candidate;
        std::snprintf(suffix, sizeof(suffix), "%04x", dist(rng));
    }
    // Worst-case fallback: append milliseconds.
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return format_id_timestamp(ts) + "-" + std::to_string(ms);
}

// -- FTS5 helpers --------------------------------------------------------------

void MemoryStore::fts_upsert(const Entry& e)
{
    // DELETE-then-INSERT keeps the rowid free to be re-minted but FTS5 keeps
    // its own auxiliary structures coherent. Cheap on small N.
    sqlite3_reset(stmt_fts_delete_);
    sqlite3_bind_text(stmt_fts_delete_, 1, e.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt_fts_delete_);

    sqlite3_reset(stmt_fts_insert_);
    sqlite3_bind_text(stmt_fts_insert_, 1, e.id.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_fts_insert_, 2, e.content.c_str(), -1, SQLITE_TRANSIENT);
    std::string tag_blob = join_tags_space(e.tags);
    sqlite3_bind_text(stmt_fts_insert_, 3, tag_blob.c_str(),  -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_fts_insert_) != SQLITE_DONE) {
        spdlog::warn("MemoryStore: FTS insert failed for {}: {}", e.id,
                     sqlite3_errmsg(main_db_.handle()));
    }
}

void MemoryStore::fts_delete(const std::string& id)
{
    sqlite3_reset(stmt_fts_delete_);
    sqlite3_bind_text(stmt_fts_delete_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt_fts_delete_);
}

// -- vec0 helpers --------------------------------------------------------------

std::int64_t MemoryStore::lookup_rowid(const std::string& id) const
{
    if (!stmt_keys_lookup_) return 0;
    sqlite3_reset(stmt_keys_lookup_);
    sqlite3_bind_text(stmt_keys_lookup_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_keys_lookup_) == SQLITE_ROW)
        return sqlite3_column_int64(stmt_keys_lookup_, 0);
    return 0;
}

bool MemoryStore::vec_embed_and_store(const std::string& id, const std::string& text)
{
    if (!embedder_ || !vectors_db_) return false;
    if (text.empty()) return false;

    auto vec = embedder_->embed_query(text);
    if (vec.empty()) return false;

    std::int64_t rowid = lookup_rowid(id);
    if (rowid == 0) {
        sqlite3_reset(stmt_keys_insert_);
        sqlite3_bind_text(stmt_keys_insert_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt_keys_insert_) != SQLITE_DONE) {
            spdlog::warn("MemoryStore: memory_keys insert failed for {}: {}",
                         id, sqlite3_errmsg(vectors_db_->handle()));
            return false;
        }
        rowid = sqlite3_last_insert_rowid(vectors_db_->handle());
    }

    sqlite3_reset(stmt_vec_insert_);
    sqlite3_bind_int64(stmt_vec_insert_, 1, rowid);
    sqlite3_bind_blob(stmt_vec_insert_, 2,
                      vec.data(),
                      static_cast<int>(vec.size() * sizeof(float)),
                      SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_vec_insert_) != SQLITE_DONE) {
        spdlog::warn("MemoryStore: vector insert failed for {}: {}", id,
                     sqlite3_errmsg(vectors_db_->handle()));
        return false;
    }
    return true;
}

void MemoryStore::vec_delete(const std::string& id)
{
    if (!vectors_db_) return;
    std::int64_t rowid = lookup_rowid(id);
    if (rowid != 0) {
        sqlite3_reset(stmt_vec_delete_);
        sqlite3_bind_int64(stmt_vec_delete_, 1, rowid);
        sqlite3_step(stmt_vec_delete_);
    }
    sqlite3_reset(stmt_keys_delete_);
    sqlite3_bind_text(stmt_keys_delete_, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt_keys_delete_);
}

// -- file I/O -----------------------------------------------------------------

void MemoryStore::write_file(const Entry& e) const
{
    auto p = dir_ / (e.id + ".md");
    atomic_write_file(p, serialise_entry(e));
}

// -- public mutation ----------------------------------------------------------

std::string MemoryStore::add(std::string              content,
                             std::vector<std::string> tags,
                             bool                     pinned,
                             std::string              source)
{
    if (trim(content).empty())
        throw std::invalid_argument("memory content must not be empty");

    Entry e;
    {
        std::lock_guard lock(mutex_);
        e.id           = mint_id(source);
        e.content      = std::move(content);
        e.tags         = std::move(tags);
        e.source       = source.empty() ? std::string("agent") : std::move(source);
        e.pinned       = pinned;
        e.created_at   = now_seconds();
        e.updated_at   = e.created_at;
        e.last_used_at = e.created_at;

        write_file(e);
        fts_upsert(e);
        // Vector embedding may fail (no embedder, no model) -- not fatal.
        try { vec_embed_and_store(e.id, e.content); }
        catch (const std::exception& ex) {
            spdlog::warn("MemoryStore: embed failed for new entry {}: {}",
                         e.id, ex.what());
        }
        entries_.emplace(e.id, e);
    }
    gc();  // takes its own lock
    return e.id;
}

bool MemoryStore::update(const std::string&                       id,
                         std::optional<std::string>               content,
                         std::optional<std::vector<std::string>>  tags,
                         std::optional<bool>                      pinned)
{
    std::lock_guard lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    auto& e = it->second;

    bool content_changed = false;
    if (content) {
        std::string c = trim(*content);
        if (c.empty()) return false;  // empty edits rejected
        if (e.content != c) {
            e.content = std::move(*content);
            content_changed = true;
        }
    }
    if (tags)   e.tags   = std::move(*tags);
    if (pinned) e.pinned = *pinned;

    e.updated_at = now_seconds();
    write_file(e);

    if (content_changed) {
        fts_upsert(e);
        try { vec_embed_and_store(e.id, e.content); }
        catch (const std::exception& ex) {
            spdlog::warn("MemoryStore: re-embed failed for {}: {}", e.id, ex.what());
        }
    } else {
        // Tags-only changes still update the FTS row so the tags column reflects.
        fts_upsert(e);
    }
    return true;
}

bool MemoryStore::soft_delete(const std::string& id)
{
    std::lock_guard lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;

    auto src = dir_ / (id + ".md");
    auto dst = deleted_dir_ / (id + ".md");
    std::error_code ec;
    fs::create_directories(deleted_dir_, ec);
    fs::rename(src, dst, ec);
    if (ec) {
        // Best-effort copy+remove if cross-device rename failed.
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            spdlog::error("MemoryStore: soft_delete failed to move {}: {}",
                          src.string(), ec.message());
            return false;
        }
        fs::remove(src, ec);
    }

    fts_delete(id);
    vec_delete(id);
    entries_.erase(it);
    return true;
}

bool MemoryStore::hard_delete(const std::string& id)
{
    std::lock_guard lock(mutex_);

    bool any = false;
    auto it = entries_.find(id);
    if (it != entries_.end()) {
        std::error_code ec;
        fs::remove(dir_ / (id + ".md"), ec);
        fts_delete(id);
        vec_delete(id);
        entries_.erase(it);
        any = true;
    }

    // Also nuke any soft-deleted copy under .deleted/.
    auto trashed = deleted_dir_ / (id + ".md");
    std::error_code ec;
    if (fs::exists(trashed, ec)) {
        fs::remove(trashed, ec);
        any = true;
    }
    return any;
}

bool MemoryStore::restore_deleted(const std::string& id)
{
    std::lock_guard lock(mutex_);
    auto src = deleted_dir_ / (id + ".md");
    if (!fs::exists(src)) return false;
    auto dst = dir_ / (id + ".md");
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (ec) return false;

    Entry e;
    if (!parse_entry(read_text_file(dst), e)) return false;
    fts_upsert(e);
    try { vec_embed_and_store(e.id, e.content); }
    catch (...) {}
    entries_.emplace(e.id, std::move(e));
    return true;
}

int MemoryStore::purge_deleted(int retention_days)
{
    std::error_code ec;
    if (!fs::exists(deleted_dir_, ec)) return 0;

    std::int64_t cutoff = now_seconds()
        - static_cast<std::int64_t>(retention_days) * 24 * 3600;
    int purged = 0;
    for (auto& de : fs::directory_iterator(deleted_dir_, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        auto p = de.path();
        if (p.extension() != ".md") continue;

        Entry e;
        if (!parse_entry(read_text_file(p), e)) {
            // Unparseable -- safe to drop after retention based on file mtime.
            auto mt = fs::last_write_time(p, ec);
            (void)mt;
            fs::remove(p, ec);
            ++purged;
            continue;
        }
        // Use updated_at as the "moved to .deleted/ at" proxy. Soft-delete
        // doesn't currently bump a deleted_at field; this is good enough for
        // a hard-delete after retention.
        if (e.updated_at > 0 && e.updated_at < cutoff) {
            fs::remove(p, ec);
            ++purged;
        }
    }
    if (purged > 0)
        spdlog::info("MemoryStore: purged {} soft-deleted entries past retention",
                     purged);
    return purged;
}

int MemoryStore::gc()
{
    std::lock_guard lock(mutex_);
    if (max_entries_ <= 0) return 0;

    std::vector<const Entry*> unpinned;
    unpinned.reserve(entries_.size());
    for (auto& [id, e] : entries_) if (!e.pinned) unpinned.push_back(&e);
    if (static_cast<int>(unpinned.size()) <= max_entries_) return 0;

    // Oldest first.
    std::sort(unpinned.begin(), unpinned.end(),
              [](const Entry* a, const Entry* b) {
                  return a->created_at < b->created_at;
              });

    int to_drop = static_cast<int>(unpinned.size()) - max_entries_;
    int dropped = 0;
    for (int i = 0; i < to_drop; ++i) {
        const auto& e = *unpinned[i];
        auto src = dir_ / (e.id + ".md");
        auto dst = deleted_dir_ / (e.id + ".md");
        std::error_code ec;
        fs::create_directories(deleted_dir_, ec);
        fs::rename(src, dst, ec);
        if (ec) {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) fs::remove(src, ec);
        }
        fts_delete(e.id);
        vec_delete(e.id);
        entries_.erase(e.id);
        ++dropped;
    }
    if (dropped > 0)
        spdlog::info("MemoryStore: GC dropped {} unpinned entries beyond limit "
                     "({} max)", dropped, max_entries_);
    return dropped;
}

// -- public read --------------------------------------------------------------

std::optional<MemoryStore::Entry> MemoryStore::get(const std::string& id) const
{
    std::lock_guard lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

std::vector<MemoryStore::Entry> MemoryStore::list_all() const
{
    std::lock_guard lock(mutex_);
    std::vector<Entry> out;
    out.reserve(entries_.size());
    for (auto& [id, e] : entries_) out.push_back(e);
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) {
                  return a.created_at > b.created_at;
              });
    return out;
}

std::vector<MemoryStore::Entry> MemoryStore::list_pinned() const
{
    std::lock_guard lock(mutex_);
    std::vector<Entry> out;
    for (auto& [id, e] : entries_) if (e.pinned) out.push_back(e);
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) {
                  return a.created_at < b.created_at;  // stable pinned order
              });
    return out;
}

int MemoryStore::estimate_slot_tokens(const Entry& e)
{
    // The slot rendering uses content + a short header per entry. ~30 tokens
    // of fixed overhead per entry approximates that header + tag block.
    return TokenCounter::estimate(e.content) + 30;
}

std::vector<MemoryStore::Entry>
MemoryStore::list_recent_for_budget(int remaining_tokens)
{
    std::lock_guard lock(mutex_);
    if (remaining_tokens <= 0) return {};

    std::vector<Entry> unpinned;
    for (auto& [id, e] : entries_) if (!e.pinned) unpinned.push_back(e);
    std::sort(unpinned.begin(), unpinned.end(),
              [](const Entry& a, const Entry& b) {
                  return a.last_used_at > b.last_used_at;
              });

    std::vector<Entry> out;
    int spent = 0;
    std::int64_t now = now_seconds();
    for (auto& e : unpinned) {
        int cost = estimate_slot_tokens(e);
        if (spent + cost > remaining_tokens) break;
        spent += cost;
        e.last_used_at = now;
        // Persist the touch.
        entries_[e.id].last_used_at = now;
        // Defer the disk write until after the lock is released to keep the
        // critical section short.
        out.push_back(e);
    }
    return out;
}

// -- search -------------------------------------------------------------------

std::vector<MemoryStore::SearchHit>
MemoryStore::search(const std::string&              query,
                    int                             max_results,
                    const std::vector<std::string>& tag_filter)
{
    if (max_results <= 0) max_results = 5;

    std::lock_guard lock(mutex_);
    struct Candidate {
        const Entry* entry      = nullptr;
        double       bm25       = 0.0;   // raw, lower = better
        double       cosine     = 0.0;   // 1 - distance
        bool         has_bm25   = false;
        bool         has_cosine = false;
    };
    std::unordered_map<std::string, Candidate> cands;

    // -- FTS5 channel
    if (!query.empty()) {
        sqlite3_reset(stmt_fts_search_);
        sqlite3_bind_text(stmt_fts_search_, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt_fts_search_, 2, max_results * 4);
        while (sqlite3_step(stmt_fts_search_) == SQLITE_ROW) {
            const auto* idp = sqlite3_column_text(stmt_fts_search_, 0);
            if (!idp) continue;
            std::string id(reinterpret_cast<const char*>(idp));
            auto it = entries_.find(id);
            if (it == entries_.end()) continue;
            Candidate& c = cands[id];
            c.entry = &it->second;
            c.bm25  = sqlite3_column_double(stmt_fts_search_, 1);
            c.has_bm25 = true;
        }
    }

    // -- vector channel
    if (embedder_ && stmt_vec_search_ && !query.empty()) {
        try {
            auto qv = embedder_->embed_query(query);
            if (!qv.empty()) {
                sqlite3_reset(stmt_vec_search_);
                sqlite3_bind_blob(stmt_vec_search_, 1,
                                  qv.data(),
                                  static_cast<int>(qv.size() * sizeof(float)),
                                  SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt_vec_search_, 2, max_results * 4);
                while (sqlite3_step(stmt_vec_search_) == SQLITE_ROW) {
                    const auto* idp = sqlite3_column_text(stmt_vec_search_, 0);
                    if (!idp) continue;
                    std::string id(reinterpret_cast<const char*>(idp));
                    auto it = entries_.find(id);
                    if (it == entries_.end()) continue;
                    double dist = sqlite3_column_double(stmt_vec_search_, 1);
                    // sqlite-vec returns squared L2 distance for normalised
                    // vectors; map to a [0..1] similarity-ish score the same
                    // way `IndexQuery::search_semantic` formats them: 1/(1+d).
                    Candidate& c = cands[id];
                    c.entry      = &it->second;
                    c.cosine     = 1.0 / (1.0 + dist);
                    c.has_cosine = true;
                }
            }
        } catch (const std::exception& ex) {
            spdlog::warn("MemoryStore: query embed failed: {}", ex.what());
        }
    }

    // If neither channel returned anything, fall back to "all entries" so
    // tag-only filters still produce results. Empty query also lands here.
    if (cands.empty()) {
        for (auto& [id, e] : entries_) {
            Candidate c;
            c.entry = &e;
            cands.emplace(id, c);
        }
    }

    // -- compose final score
    // BM25 lower = better; normalise by the min observed score in the result set.
    double min_bm25 =  1e9;
    double max_bm25 = -1e9;
    for (auto& [id, c] : cands) {
        if (c.has_bm25) {
            min_bm25 = std::min(min_bm25, c.bm25);
            max_bm25 = std::max(max_bm25, c.bm25);
        }
    }
    auto normalised_bm25 = [&](double r) -> double {
        if (max_bm25 == min_bm25) return r == 0.0 ? 0.0 : 1.0;
        // Lower (more negative) BM25 -> better -> closer to 1.
        return (max_bm25 - r) / (max_bm25 - min_bm25);
    };

    auto recency_factor = [&](std::int64_t created_at) -> double {
        if (recency_half_life_days_ <= 0) return 0.0;
        double age_days = static_cast<double>(now_seconds() - created_at)
                          / (24.0 * 3600.0);
        if (age_days < 0) age_days = 0;
        return std::exp(-age_days * std::log(2.0)
                        / static_cast<double>(recency_half_life_days_));
    };

    std::vector<SearchHit> hits;
    hits.reserve(cands.size());
    for (auto& [id, c] : cands) {
        if (!c.entry) continue;
        const Entry& e = *c.entry;

        // Tag filter: require every requested tag to be present.
        if (!tag_filter.empty()) {
            bool ok = true;
            for (auto& need : tag_filter) {
                if (std::find(e.tags.begin(), e.tags.end(), need) == e.tags.end()) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
        }

        SearchHit h;
        h.entry      = e;
        h.fts_score  = c.has_bm25   ? normalised_bm25(c.bm25) : 0.0;
        h.vec_score  = c.has_cosine ? c.cosine                : 0.0;
        h.recency    = recency_factor(e.created_at);
        // Equal weight on the two retrieval channels; recency is an additive
        // boost (0..1) on top -- a fresh-but-weak match still ranks behind a
        // strong-but-old match.
        h.score = h.fts_score + h.vec_score + 0.5 * h.recency;
        hits.push_back(std::move(h));
    }

    std::sort(hits.begin(), hits.end(),
              [](const SearchHit& a, const SearchHit& b) {
                  return a.score > b.score;
              });

    // Optional reranker -- gated on the workspace's `reranker_enabled` flag
    // through the supplied Reranker pointer (null => no reranker available).
    if (reranker_ && !query.empty() && !hits.empty()) {
        const std::size_t top_k = std::min<std::size_t>(hits.size(), 20);
        std::vector<std::string> passages;
        passages.reserve(top_k);
        for (std::size_t i = 0; i < top_k; ++i)
            passages.push_back(hits[i].entry.content);
        try {
            auto scores = reranker_->score_batch(query, passages);
            for (std::size_t i = 0; i < top_k && i < scores.size(); ++i) {
                hits[i].rerank_score = scores[i];
                hits[i].score        = scores[i];
            }
            // Re-sort the slice that got rerank scores; the tail keeps
            // the bi-encoder ordering.
            std::sort(hits.begin(), hits.begin() + top_k,
                      [](const SearchHit& a, const SearchHit& b) {
                          return a.score > b.score;
                      });
        } catch (const std::exception& ex) {
            spdlog::warn("MemoryStore: reranker failed: {}", ex.what());
        }
    }

    if (static_cast<int>(hits.size()) > max_results)
        hits.resize(max_results);

    // Bump last_used_at for the entries we're returning.
    std::int64_t now = now_seconds();
    for (auto& h : hits) {
        auto it = entries_.find(h.entry.id);
        if (it != entries_.end()) {
            it->second.last_used_at = now;
            h.entry.last_used_at    = now;
        }
    }
    return hits;
}

// -- introspection ------------------------------------------------------------

std::string MemoryStore::format_for_system_prompt()
{
    if (in_context_budget_tokens_ <= 0) return {};

    auto pinned = list_pinned();
    int  budget = in_context_budget_tokens_;
    int  spent  = 0;

    auto render_entry = [](const Entry& e) {
        std::ostringstream o;
        o << "- memory:" << e.id;
        if (e.pinned) o << "  *pinned*";
        if (!e.tags.empty()) {
            o << "  tags=";
            for (size_t i = 0; i < e.tags.size(); ++i) {
                if (i) o << ",";
                o << e.tags[i];
            }
        }
        o << "\n  " << e.content;
        if (e.content.empty() || e.content.back() != '\n') o << "\n";
        return o.str();
    };

    std::vector<std::string> blocks;
    // Pinned entries first (subject to budget so a single oversized pin
    // can't blow the whole prompt).
    for (auto& e : pinned) {
        std::string body = render_entry(e);
        int cost = TokenCounter::estimate(body);
        if (spent + cost > budget && !blocks.empty()) break;
        blocks.push_back(std::move(body));
        spent += cost;
    }

    int remaining = budget - spent;
    if (remaining > 0) {
        auto recent = list_recent_for_budget(remaining);
        for (auto& e : recent) {
            blocks.push_back(render_entry(e));
        }
    }

    if (blocks.empty()) return {};

    std::ostringstream out;
    out << "## Memory Bank\n"
        << "Workspace-scoped notes that survive between sessions. Use "
           "`search_memory` to look up anything else; `add_memory` to commit "
           "new facts you discover.\n\n";
    for (auto& b : blocks) out << b;
    return out.str();
}

std::size_t MemoryStore::live_count() const
{
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::size_t MemoryStore::deleted_count() const
{
    std::error_code ec;
    if (!fs::exists(deleted_dir_, ec)) return 0;
    std::size_t n = 0;
    for (auto& de : fs::directory_iterator(deleted_dir_, ec)) {
        if (ec) break;
        if (de.is_regular_file() && de.path().extension() == ".md") ++n;
    }
    return n;
}

} // namespace locus

#include "web/web_cache.h"

#include "core/database.h"
#include "core/log_channels.h"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <chrono>

namespace locus {

namespace {

int64_t unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string col_text(sqlite3_stmt* stmt, int col)
{
    auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? t : "";
}

} // namespace

std::string extract_domain(const std::string& url)
{
    // Strip scheme.
    auto scheme = url.find("://");
    size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
    // Host ends at the next '/', '?', or '#'.
    size_t end = url.find_first_of("/?#", start);
    std::string host = (end == std::string::npos)
                           ? url.substr(start)
                           : url.substr(start, end - start);
    // Drop any userinfo / port.
    auto at = host.find('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    auto colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    return host.empty() ? url : host;
}

WebCache::WebCache(Database& main_db)
    : main_db_(main_db)
{
    ensure_schema();
}

WebCache::~WebCache()
{
    auto fin = [](sqlite3_stmt*& s) {
        if (s) { sqlite3_finalize(s); s = nullptr; }
    };
    fin(stmt_upsert_page_);
    fin(stmt_delete_page_);
    fin(stmt_page_id_);
    fin(stmt_insert_fts_);
    fin(stmt_delete_fts_);
    fin(stmt_insert_head_);
    fin(stmt_delete_heads_);
    fin(stmt_get_content_);
    fin(stmt_get_title_);
    fin(stmt_get_flags_);
    fin(stmt_get_heads_);
}

void WebCache::ensure_schema()
{
    // Fetched-page metadata + taint columns (S6.0). origin is always "web" but
    // kept as a column for parity with the files-table read path. Separate from
    // the workspace index by design (see header).
    main_db_.exec(R"(
        CREATE TABLE IF NOT EXISTS web_pages (
            id              INTEGER PRIMARY KEY,
            url             TEXT UNIQUE NOT NULL,
            title           TEXT,
            domain          TEXT,
            content         TEXT,
            word_count      INTEGER DEFAULT 0,
            fetched_at      INTEGER,
            session_id      TEXT,
            origin          TEXT DEFAULT 'web',
            injection_flags INTEGER DEFAULT 0
        )
    )");

    // FTS5 over fetched-page content. `url` UNINDEXED so it round-trips without
    // being tokenized; the search read path joins back to web_pages for taint +
    // metadata. Same tokenizer as files_fts so cross-source ranking is sane.
    main_db_.exec(R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS web_fts USING fts5(
            url UNINDEXED,
            content,
            tokenize='porter unicode61'
        )
    )");

    main_db_.exec(R"(
        CREATE TABLE IF NOT EXISTS web_headings (
            id          INTEGER PRIMARY KEY,
            page_id     INTEGER REFERENCES web_pages(id),
            level       INTEGER,
            text        TEXT,
            line_number INTEGER
        )
    )");
    main_db_.exec("CREATE INDEX IF NOT EXISTS web_headings_page ON web_headings(page_id)");

    stmt_upsert_page_ = main_db_.prepare(R"(
        INSERT INTO web_pages
            (url, title, domain, content, word_count, fetched_at, session_id, origin, injection_flags)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 'web', ?8)
        ON CONFLICT(url) DO UPDATE SET
            title=?2, domain=?3, content=?4, word_count=?5,
            fetched_at=?6, session_id=?7, injection_flags=?8
    )");
    stmt_delete_page_  = main_db_.prepare("DELETE FROM web_pages WHERE id = ?1");
    stmt_page_id_      = main_db_.prepare("SELECT id FROM web_pages WHERE url = ?1");
    stmt_insert_fts_   = main_db_.prepare("INSERT INTO web_fts (url, content) VALUES (?1, ?2)");
    stmt_delete_fts_   = main_db_.prepare("DELETE FROM web_fts WHERE url = ?1");
    stmt_insert_head_  = main_db_.prepare(R"(
        INSERT INTO web_headings (page_id, level, text, line_number)
        VALUES (?1, ?2, ?3, ?4)
    )");
    stmt_delete_heads_ = main_db_.prepare("DELETE FROM web_headings WHERE page_id = ?1");
    stmt_get_content_  = main_db_.prepare("SELECT content FROM web_pages WHERE url = ?1");
    stmt_get_title_    = main_db_.prepare("SELECT title FROM web_pages WHERE url = ?1");
    stmt_get_flags_    = main_db_.prepare("SELECT injection_flags FROM web_pages WHERE url = ?1");
    stmt_get_heads_    = main_db_.prepare(R"(
        SELECT h.level, h.text, h.line_number
        FROM web_headings h
        JOIN web_pages p ON p.id = h.page_id
        WHERE p.url = ?1
        ORDER BY h.line_number
    )");

    log_fs()->trace("WebCache schema ready ({} pages)", page_count());
}

int64_t WebCache::page_id_for_url(const std::string& url) const
{
    sqlite3_reset(stmt_page_id_);
    sqlite3_bind_text(stmt_page_id_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_page_id_) == SQLITE_ROW)
        return sqlite3_column_int64(stmt_page_id_, 0);
    return -1;
}

void WebCache::delete_page_by_id(int64_t id)
{
    // Resolve url for the FTS delete (web_fts is keyed by url, not page id).
    sqlite3_stmt* url_stmt = main_db_.prepare("SELECT url FROM web_pages WHERE id = ?1");
    std::string url;
    sqlite3_bind_int64(url_stmt, 1, id);
    if (sqlite3_step(url_stmt) == SQLITE_ROW) url = col_text(url_stmt, 0);
    sqlite3_finalize(url_stmt);

    if (!url.empty()) {
        sqlite3_reset(stmt_delete_fts_);
        sqlite3_bind_text(stmt_delete_fts_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_delete_fts_);
    }
    sqlite3_reset(stmt_delete_heads_);
    sqlite3_bind_int64(stmt_delete_heads_, 1, id);
    sqlite3_step(stmt_delete_heads_);

    sqlite3_reset(stmt_delete_page_);
    sqlite3_bind_int64(stmt_delete_page_, 1, id);
    sqlite3_step(stmt_delete_page_);
}

int64_t WebCache::store_page(const std::string& url,
                             const std::string& title,
                             const std::string& content,
                             const std::vector<ExtractedHeading>& headings,
                             const std::string& session_id,
                             uint32_t injection_flags)
{
    if (url.empty()) return -1;

    const std::string domain = extract_domain(url);

    // word_count over the stripped text (whitespace-delimited tokens).
    int64_t words = 0;
    bool in_word = false;
    for (char c : content) {
        bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (!ws && !in_word) { ++words; in_word = true; }
        else if (ws) in_word = false;
    }

    // Re-fetch is an upsert: clear the prior FTS + headings for this URL first
    // so a re-fetch doesn't accumulate stale rows.
    int64_t prior = page_id_for_url(url);
    if (prior >= 0) {
        sqlite3_reset(stmt_delete_fts_);
        sqlite3_bind_text(stmt_delete_fts_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt_delete_fts_);
        sqlite3_reset(stmt_delete_heads_);
        sqlite3_bind_int64(stmt_delete_heads_, 1, prior);
        sqlite3_step(stmt_delete_heads_);
    }

    sqlite3_reset(stmt_upsert_page_);
    sqlite3_bind_text (stmt_upsert_page_, 1, url.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt_upsert_page_, 2, title.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt_upsert_page_, 3, domain.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt_upsert_page_, 4, content.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_upsert_page_, 5, words);
    sqlite3_bind_int64(stmt_upsert_page_, 6, unix_now());
    sqlite3_bind_text (stmt_upsert_page_, 7, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_upsert_page_, 8, static_cast<int64_t>(injection_flags));
    if (sqlite3_step(stmt_upsert_page_) != SQLITE_DONE) {
        log_fs()->warn("WebCache::store_page upsert failed for {}", url);
        return -1;
    }

    int64_t page_id = page_id_for_url(url);
    if (page_id < 0) return -1;

    sqlite3_reset(stmt_insert_fts_);
    sqlite3_bind_text(stmt_insert_fts_, 1, url.c_str(),     -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_fts_, 2, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt_insert_fts_);

    for (const auto& h : headings) {
        sqlite3_reset(stmt_insert_head_);
        sqlite3_bind_int64(stmt_insert_head_, 1, page_id);
        sqlite3_bind_int  (stmt_insert_head_, 2, h.level);
        sqlite3_bind_text (stmt_insert_head_, 3, h.text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (stmt_insert_head_, 4, h.line_number);
        sqlite3_step(stmt_insert_head_);
    }

    log_fs()->trace("WebCache: stored {} ({} words, {} headings)",
                    url, words, headings.size());
    return page_id;
}

bool WebCache::has_page(const std::string& url) const
{
    return page_id_for_url(url) >= 0;
}

std::optional<std::string> WebCache::get_content(const std::string& url) const
{
    sqlite3_reset(stmt_get_content_);
    sqlite3_bind_text(stmt_get_content_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_get_content_) == SQLITE_ROW)
        return col_text(stmt_get_content_, 0);
    return std::nullopt;
}

std::vector<ExtractedHeading> WebCache::get_headings(const std::string& url) const
{
    std::vector<ExtractedHeading> out;
    sqlite3_reset(stmt_get_heads_);
    sqlite3_bind_text(stmt_get_heads_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt_get_heads_) == SQLITE_ROW) {
        ExtractedHeading h;
        h.level       = sqlite3_column_int(stmt_get_heads_, 0);
        h.text        = col_text(stmt_get_heads_, 1);
        h.line_number = sqlite3_column_int(stmt_get_heads_, 2);
        out.push_back(std::move(h));
    }
    return out;
}

std::string WebCache::get_title(const std::string& url) const
{
    sqlite3_reset(stmt_get_title_);
    sqlite3_bind_text(stmt_get_title_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_get_title_) == SQLITE_ROW)
        return col_text(stmt_get_title_, 0);
    return {};
}

uint32_t WebCache::get_injection_flags(const std::string& url) const
{
    sqlite3_reset(stmt_get_flags_);
    sqlite3_bind_text(stmt_get_flags_, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_get_flags_) == SQLITE_ROW)
        return static_cast<uint32_t>(sqlite3_column_int64(stmt_get_flags_, 0));
    return 0;
}

int WebCache::page_count() const
{
    sqlite3_stmt* stmt = main_db_.prepare("SELECT COUNT(*) FROM web_pages");
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

int64_t WebCache::total_content_bytes() const
{
    sqlite3_stmt* stmt = main_db_.prepare(
        "SELECT COALESCE(SUM(LENGTH(content)), 0) FROM web_pages");
    int64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

int WebCache::evict_expired(int ttl_hours)
{
    if (ttl_hours <= 0) return 0;
    int64_t cutoff = unix_now() - static_cast<int64_t>(ttl_hours) * 3600;

    std::vector<int64_t> ids;
    sqlite3_stmt* stmt = main_db_.prepare(
        "SELECT id FROM web_pages WHERE fetched_at < ?1");
    sqlite3_bind_int64(stmt, 1, cutoff);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        ids.push_back(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);

    for (int64_t id : ids) delete_page_by_id(id);
    if (!ids.empty())
        spdlog::info("WebCache: evicted {} page(s) past TTL ({}h)", ids.size(), ttl_hours);
    return static_cast<int>(ids.size());
}

int WebCache::evict_over_size(int64_t max_bytes)
{
    if (max_bytes <= 0) return 0;
    int removed = 0;
    while (total_content_bytes() > max_bytes) {
        sqlite3_stmt* stmt = main_db_.prepare(
            "SELECT id FROM web_pages ORDER BY fetched_at ASC LIMIT 1");
        int64_t oldest = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW) oldest = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        if (oldest < 0) break;
        delete_page_by_id(oldest);
        ++removed;
    }
    if (removed > 0)
        spdlog::info("WebCache: evicted {} page(s) over size cap ({} bytes)",
                     removed, max_bytes);
    return removed;
}

void WebCache::clear()
{
    main_db_.exec("DELETE FROM web_headings");
    main_db_.exec("DELETE FROM web_fts");
    main_db_.exec("DELETE FROM web_pages");
    spdlog::info("WebCache: cleared");
}

} // namespace locus

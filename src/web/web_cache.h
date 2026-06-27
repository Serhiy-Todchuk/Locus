#pragma once

#include "extractors/text_extractor.h"  // ExtractedHeading

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3_stmt;

namespace locus {

class Database;

// S6.1 -- ephemeral store for fetched web pages.
//
// Web content is NOT the workspace index. It lives in its own tables
// (web_pages / web_fts / web_headings) in the main index.db so the agent can
// search local + web together when asked, but with a separate lifecycle: pages
// are scoped to a session, evicted past a TTL, and capped by a total disk
// budget. Deleting web_* never touches the real workspace index.
//
// The tables carry the S6.0 taint columns (origin = "web", injection_flags = a
// bitmask of InjectionCategory bits that fired at fetch). The search read path
// stamps an origin marker (and re-wraps flagged snippets) the same way it does
// for ZIM / MCP ingress.
//
// All schema is created LAZILY on construction (cheap -- empty FTS5 + two small
// tables) but the heavy work (fetch + extraction) is the caller's job; this
// class only persists, reads back, and evicts. Single-threaded use is assumed
// (the agent thread); it shares the main_db connection with the indexer, so
// callers must serialise around index writes the same way other main_db users
// do (in practice fetch happens between LLM rounds, never concurrent with a
// watcher flush).
class WebCache {
public:
    // A search-API result row (web_search), pre-fetch. Not persisted -- this is
    // the compact thing the tool returns to the LLM.
    struct SearchHit {
        std::string title;
        std::string url;
        std::string snippet;
    };

    // Outline of a fetched page, returned by web_fetch (compact, ~30 tokens).
    struct PageOutline {
        std::string url;
        std::string title;
        std::string domain;
        int64_t     word_count = 0;
        std::vector<ExtractedHeading> headings;
        // Taint surface (S6.0). injection_banner is non-empty when the policy
        // wrapped/escalated the body; the tool forwards it to the activity log.
        std::string injection_banner;
        uint32_t    injection_flags = 0;
    };

    explicit WebCache(Database& main_db);
    ~WebCache();

    WebCache(const WebCache&) = delete;
    WebCache& operator=(const WebCache&) = delete;

    // Persist a fetched + extracted page. `content` is the already-stripped
    // plain text (post-injection-policy: it may already carry a nonce fence).
    // `injection_flags` is the bitmask recorded at ingress. Upserts on URL
    // (re-fetching the same URL replaces the prior row + its FTS / headings).
    // Returns the page id, or -1 on failure.
    int64_t store_page(const std::string& url,
                       const std::string& title,
                       const std::string& content,
                       const std::vector<ExtractedHeading>& headings,
                       const std::string& session_id,
                       uint32_t injection_flags);

    // True if the URL is already in the cache (and not TTL-expired).
    bool has_page(const std::string& url) const;

    // Read the stored plain text of a page by URL. nullopt if absent.
    std::optional<std::string> get_content(const std::string& url) const;

    // Read the headings of a page by URL (sorted by line number).
    std::vector<ExtractedHeading> get_headings(const std::string& url) const;

    // Read the stored title of a page by URL. Empty if absent / untitled.
    std::string get_title(const std::string& url) const;

    // The injection flag bitmask recorded for a page at fetch (0 = clean / absent).
    uint32_t get_injection_flags(const std::string& url) const;

    // Number of pages currently cached (post-eviction).
    int page_count() const;

    // Approximate on-disk size of cached page content, in bytes (sum of
    // content lengths -- a proxy for the disk budget, not exact page count).
    int64_t total_content_bytes() const;

    // TTL eviction: delete pages older than `ttl_hours`. Returns the count
    // removed. ttl_hours <= 0 disables (no-op).
    int evict_expired(int ttl_hours);

    // Size-cap eviction: while total_content_bytes() exceeds `max_bytes`,
    // delete the oldest page. Returns the count removed. max_bytes <= 0
    // disables (no-op).
    int evict_over_size(int64_t max_bytes);

    // Drop every cached page (manual "clear web cache").
    void clear();

private:
    void ensure_schema();
    int64_t page_id_for_url(const std::string& url) const;
    void delete_page_by_id(int64_t id);

    Database& main_db_;

    // Prepared statements (lazily prepared in ensure_schema()).
    sqlite3_stmt* stmt_upsert_page_     = nullptr;
    sqlite3_stmt* stmt_delete_page_     = nullptr;
    sqlite3_stmt* stmt_page_id_         = nullptr;
    sqlite3_stmt* stmt_insert_fts_      = nullptr;
    sqlite3_stmt* stmt_delete_fts_      = nullptr;
    sqlite3_stmt* stmt_insert_head_     = nullptr;
    sqlite3_stmt* stmt_delete_heads_    = nullptr;
    sqlite3_stmt* stmt_get_content_     = nullptr;
    sqlite3_stmt* stmt_get_title_       = nullptr;
    sqlite3_stmt* stmt_get_flags_       = nullptr;
    sqlite3_stmt* stmt_get_heads_       = nullptr;
};

// Extract the registrable domain (host) from a URL for the web_pages.domain
// display column. "https://en.wikipedia.org/wiki/X" -> "en.wikipedia.org".
// Returns the whole input on a shape it doesn't recognise. Pure; unit-tested.
std::string extract_domain(const std::string& url);

} // namespace locus

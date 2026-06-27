#pragma once

#include "web/web_cache.h"  // WebCache::SearchHit

#include <string>
#include <vector>

namespace locus {

// S6.1 -- web search + fetch over cpr. Two thin, provider-pluggable surfaces:
//
//   web_search  -> a configurable search API (Brave default, SearXNG adapter)
//                  returns compact {title, url, snippet} rows.
//   web_fetch   -> a plain HTTP(S) GET of a single page -> raw bytes.
//
// Both are pure-network helpers: no DB, no extraction, no injection scanning.
// The web tools own the policy (approval, scan, store). Kept separate from the
// LLM transport so a flaky search key never touches the chat path.

// Settings the provider needs -- a decoupled copy of the relevant
// WorkspaceConfig::Web fields so this module doesn't pull in the config header.
struct WebSearchSettings {
    std::string provider = "brave";       // "brave" | "searxng"
    std::string api_key;
    std::string api_url;
    int         max_results   = 5;
    int         timeout_ms    = 15000;     // per-request total timeout
    bool        allow_http    = false;     // HTTPS-only unless set
};

struct WebSearchResponse {
    bool                          ok = false;
    std::string                   error;   // human-facing reason on failure
    std::vector<WebCache::SearchHit> hits;
};

struct WebFetchResponse {
    bool        ok = false;
    long        status_code = 0;
    std::string error;        // human-facing reason on failure
    std::string body;         // raw response body (HTML/text) on success
    std::string content_type; // response Content-Type header (lowercased)
    std::string final_url;    // after redirects
};

// Run a search-API query. Validates the URL scheme against allow_http, sends
// the provider-specific request, parses the JSON, and returns up to
// max_results hits. On any failure (no key, network error, bad JSON) returns
// ok=false with a populated `error`.
WebSearchResponse web_search_query(const std::string& query,
                                   const WebSearchSettings& settings);

// HTTP(S) GET of a single page. Follows redirects (capped), enforces the
// scheme policy, and identifies as Locus (not a browser). Returns the raw body
// on 2xx; non-2xx is ok=false with the status code populated.
WebFetchResponse web_fetch_url(const std::string& url,
                               const WebSearchSettings& settings);

// Parse a Brave Search API JSON response body into hits. Exposed for tests so
// the JSON adapter is verifiable without a network round-trip. Tolerant of a
// missing `web.results` array (returns empty).
std::vector<WebCache::SearchHit> parse_brave_results(const std::string& json_body,
                                                     int max_results);

// Parse a SearXNG JSON response (`{"results":[{title,url,content}, ...]}`).
std::vector<WebCache::SearchHit> parse_searxng_results(const std::string& json_body,
                                                       int max_results);

} // namespace locus

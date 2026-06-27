#include "web/web_search_provider.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace locus {

using json = nlohmann::json;

namespace {

bool is_https(const std::string& url)
{
    return url.size() >= 8 &&
           (url.compare(0, 8, "https://") == 0);
}

bool is_http(const std::string& url)
{
    return url.size() >= 7 &&
           (url.compare(0, 7, "http://") == 0);
}

// Scheme gate shared by search + fetch. Returns empty on success or a reason.
std::string scheme_error(const std::string& url, bool allow_http)
{
    if (is_https(url)) return {};
    if (is_http(url)) {
        if (allow_http) return {};
        return "Refusing http:// URL (set web.allow_http to override): " + url;
    }
    return "Only http(s) URLs are supported: " + url;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::vector<WebCache::SearchHit> parse_brave_results(const std::string& body,
                                                     int max_results)
{
    std::vector<WebCache::SearchHit> hits;
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return hits;

    // Brave shape: { "web": { "results": [ { title, url, description }, ... ] } }
    if (!j.contains("web") || !j["web"].is_object()) return hits;
    const auto& web = j["web"];
    if (!web.contains("results") || !web["results"].is_array()) return hits;

    for (const auto& r : web["results"]) {
        if (!r.is_object()) continue;
        WebCache::SearchHit h;
        if (r.contains("title") && r["title"].is_string())
            h.title = r["title"].get<std::string>();
        if (r.contains("url") && r["url"].is_string())
            h.url = r["url"].get<std::string>();
        if (r.contains("description") && r["description"].is_string())
            h.snippet = r["description"].get<std::string>();
        if (h.url.empty()) continue;
        hits.push_back(std::move(h));
        if (static_cast<int>(hits.size()) >= max_results) break;
    }
    return hits;
}

std::vector<WebCache::SearchHit> parse_searxng_results(const std::string& body,
                                                       int max_results)
{
    std::vector<WebCache::SearchHit> hits;
    json j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return hits;

    // SearXNG shape: { "results": [ { title, url, content }, ... ] }
    if (!j.contains("results") || !j["results"].is_array()) return hits;
    for (const auto& r : j["results"]) {
        if (!r.is_object()) continue;
        WebCache::SearchHit h;
        if (r.contains("title") && r["title"].is_string())
            h.title = r["title"].get<std::string>();
        if (r.contains("url") && r["url"].is_string())
            h.url = r["url"].get<std::string>();
        if (r.contains("content") && r["content"].is_string())
            h.snippet = r["content"].get<std::string>();
        if (h.url.empty()) continue;
        hits.push_back(std::move(h));
        if (static_cast<int>(hits.size()) >= max_results) break;
    }
    return hits;
}

WebSearchResponse web_search_query(const std::string& query,
                                   const WebSearchSettings& settings)
{
    WebSearchResponse out;

    if (query.empty()) {
        out.error = "Empty search query";
        return out;
    }
    const std::string provider = lower(settings.provider);
    if (settings.api_url.empty()) {
        out.error = "web.api_url is not configured";
        return out;
    }
    if (auto e = scheme_error(settings.api_url, settings.allow_http); !e.empty()) {
        out.error = e;
        return out;
    }
    if (provider == "brave" && settings.api_key.empty()) {
        out.error = "Brave Search requires web.api_key (get one at "
                    "https://brave.com/search/api/)";
        return out;
    }

    cpr::Header headers;
    headers["Accept"]     = "application/json";
    headers["User-Agent"] = "Locus/1.0 (+local agent)";
    cpr::Parameters params{{"q", query}};

    if (provider == "brave") {
        headers["X-Subscription-Token"] = settings.api_key;
        headers["Accept-Encoding"]      = "gzip";
        // Parenthesise std::max so a leaked Windows max() macro (cpr pulls in
        // <windows.h>) doesn't try to macro-expand it.
        params.Add(cpr::Parameter{"count",
            std::to_string((std::max)(1, settings.max_results))});
    } else if (provider == "searxng") {
        params.Add(cpr::Parameter{"format", "json"});
        if (!settings.api_key.empty())
            headers["Authorization"] = "Bearer " + settings.api_key;
    } else {
        out.error = "Unknown search_provider '" + settings.provider +
                    "' (expected 'brave' or 'searxng')";
        return out;
    }

    cpr::Response resp = cpr::Get(
        cpr::Url{settings.api_url},
        headers,
        params,
        cpr::ConnectTimeout{10000},
        cpr::Timeout{settings.timeout_ms},
        cpr::Redirect{5L, true, false, cpr::PostRedirectFlags::POST_ALL});

    if (resp.error.code != cpr::ErrorCode::OK) {
        out.error = "Search request failed: " + resp.error.message;
        spdlog::warn("web_search: {} ({})", out.error,
                     static_cast<int>(resp.error.code));
        return out;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        out.error = "Search API returned HTTP " + std::to_string(resp.status_code);
        spdlog::warn("web_search: {}", out.error);
        return out;
    }

    out.hits = (provider == "brave")
                   ? parse_brave_results(resp.text, settings.max_results)
                   : parse_searxng_results(resp.text, settings.max_results);
    out.ok = true;
    spdlog::trace("web_search: '{}' -> {} hits", query, out.hits.size());
    return out;
}

WebFetchResponse web_fetch_url(const std::string& url,
                               const WebSearchSettings& settings)
{
    WebFetchResponse out;

    if (url.empty()) {
        out.error = "Empty URL";
        return out;
    }
    if (auto e = scheme_error(url, settings.allow_http); !e.empty()) {
        out.error = e;
        return out;
    }

    cpr::Header headers;
    headers["User-Agent"] = "Locus/1.0 (+local agent)";
    headers["Accept"]     = "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8";

    cpr::Response resp = cpr::Get(
        cpr::Url{url},
        headers,
        cpr::ConnectTimeout{10000},
        cpr::Timeout{settings.timeout_ms},
        cpr::Redirect{5L, true, false, cpr::PostRedirectFlags::POST_ALL});

    out.status_code = resp.status_code;
    out.final_url   = resp.url.str();

    if (resp.error.code != cpr::ErrorCode::OK) {
        out.error = "Fetch failed: " + resp.error.message;
        spdlog::warn("web_fetch: {} ({})", out.error,
                     static_cast<int>(resp.error.code));
        return out;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        out.error = "Fetch returned HTTP " + std::to_string(resp.status_code);
        return out;
    }

    auto ct = resp.header.find("Content-Type");
    if (ct != resp.header.end()) out.content_type = lower(ct->second);

    out.body = std::move(resp.text);
    out.ok = true;
    spdlog::trace("web_fetch: {} -> {} bytes (HTTP {})",
                  url, out.body.size(), resp.status_code);
    return out;
}

} // namespace locus

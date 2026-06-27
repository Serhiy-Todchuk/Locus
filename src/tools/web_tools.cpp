#include "tools/web_tools.h"
#include "tools/shared.h"

#include "core/workspace.h"
#include "core/workspace_services.h"
#include "extractors/html_extractor.h"
#include "security/injection_policy.h"
#include "security/injection_scanner.h"
#include "web/web_cache.h"
#include "web/web_search_provider.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace locus {

using tools::error_result;

namespace {

// Master gate: the web_retrieval capability MUST be on. available() also needs
// the WebCache wired (Workspace creates it only when the capability is on, so
// the two move together, but check both for robustness in tests).
bool web_available(IWorkspaceServices& ws)
{
    if (auto* w = ws.workspace()) {
        if (!w->config().capabilities.web_retrieval) return false;
    }
    return ws.web_cache() != nullptr;
}

// Build the network settings from the workspace config. Returns false (with a
// reason in `err`) when web is hard-disabled at runtime.
bool make_settings(IWorkspaceServices& ws, WebSearchSettings& out, std::string& err)
{
    auto* w = ws.workspace();
    if (!w) { err = "web tools require a workspace"; return false; }
    const auto& wc = w->config().web;
    if (!wc.enabled) {
        err = "Web retrieval is disabled (set web.enabled in .locus/config.json "
              "or the Settings > Web tab).";
        return false;
    }
    out.provider    = wc.search_provider;
    out.api_key     = wc.api_key;
    out.api_url     = wc.api_url;
    out.max_results = wc.max_results;
    out.allow_http  = wc.allow_http;
    return true;
}

// Scan + apply the S6.0 ingress policy to freshly fetched text. Mutates
// `content` (may wrap it in a nonce fence), returns the bitmask + banner.
void scan_ingress(IWorkspaceServices& ws, std::string& content,
                  uint32_t& flags_out, std::string& banner_out)
{
    flags_out = 0;
    banner_out.clear();
    auto* w = ws.workspace();
    if (!w) return;
    const auto& s = w->config().security;
    if (!s.injection_scan || content.empty()) return;

    security::ScannerConfig scfg;
    scfg.max_scan_bytes = static_cast<std::size_t>(s.max_scan_kb) * 1024;
    security::ScanResult scan = security::scan_for_injection(content, scfg);
    flags_out = scan.flags_bitmask();

    security::SecurityConfig sec;
    sec.injection_scan   = s.injection_scan;
    sec.scan_zim         = s.scan_zim;
    sec.block_confidence = s.block_confidence;
    sec.max_scan_kb      = s.max_scan_kb;
    security::IngressDecision dec =
        security::apply_injection_policy(content, scan, sec);
    if (dec.action != security::IngressAction::Pass)
        banner_out = dec.banner;
}

std::string preview_text(const std::string& s, std::size_t cap = 80)
{
    if (s.size() <= cap) return s;
    return s.substr(0, cap - 3) + "...";
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

// -- WebSearchTool ----------------------------------------------------------

bool WebSearchTool::available(IWorkspaceServices& ws) const { return web_available(ws); }

std::string WebSearchTool::preview(const ToolCall& call) const
{
    std::string q = preview_text(tools::coerce_string(call.args, "query"));
    return q.empty() ? std::string("web_search") : ("web_search: " + q);
}

ToolResult WebSearchTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                  const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call, {"query", "max_results"}, this))
        return *err;

    std::string query = tools::coerce_string(call.args, "query");
    if (query.empty())
        return tools::missing_required_arg(*this, "query", "the search phrase");

    WebSearchSettings settings;
    std::string err;
    if (!make_settings(ws, settings, err)) return error_result("Error: " + err);

    int requested = static_cast<int>(tools::coerce_int(call.args, "max_results",
                                                       settings.max_results));
    if (requested > 0) settings.max_results = requested;

    WebSearchResponse resp = web_search_query(query, settings);
    if (!resp.ok)
        return error_result("Web search failed: " + resp.error);

    std::ostringstream out;
    out << resp.hits.size() << " web result(s) for \"" << query << "\"\n";
    for (const auto& h : resp.hits) {
        out << "- " << (h.title.empty() ? h.url : h.title) << "\n";
        out << "  " << h.url << "\n";
        if (!h.snippet.empty()) out << "  " << h.snippet << "\n";
    }
    if (resp.hits.empty())
        out << "(no results -- try different keywords)\n";
    else
        out << "\nFetch a result with web_fetch(url) to read it.\n";

    std::string body = out.str();
    return {true, body, body};
}

// -- WebFetchTool -----------------------------------------------------------

bool WebFetchTool::available(IWorkspaceServices& ws) const { return web_available(ws); }

std::string WebFetchTool::preview(const ToolCall& call) const
{
    std::string u = preview_text(tools::coerce_string(call.args, "url"), 100);
    return u.empty() ? std::string("web_fetch") : ("web_fetch: " + u);
}

ToolResult WebFetchTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                 const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call, {"url"}, this))
        return *err;

    std::string url = tools::coerce_string(call.args, "url");
    if (url.empty())
        return tools::missing_required_arg(*this, "url", "absolute http(s) URL");

    auto* cache = ws.web_cache();
    if (!cache) return error_result("Error: web cache not available");

    WebSearchSettings settings;
    std::string err;
    if (!make_settings(ws, settings, err)) return error_result("Error: " + err);

    WebFetchResponse resp = web_fetch_url(url, settings);
    if (!resp.ok)
        return error_result("Fetch failed: " + resp.error);

    // Extract readable text. HTML goes through gumbo; anything else (text/plain,
    // JSON) is stored verbatim with no headings.
    ExtractionResult ext;
    bool is_html = resp.content_type.find("html") != std::string::npos ||
                   (resp.content_type.empty() &&
                    resp.body.find("<html") != std::string::npos);
    if (is_html) {
        ext = HtmlExtractor::extract_from_string(resp.body);
    } else {
        ext.text = resp.body;
    }

    // Cap extracted text per the config (default 512 KB) so a giant page can't
    // bloat the cache.
    const auto& wc = ws.workspace()->config().web;
    std::size_t cap = static_cast<std::size_t>(wc.max_web_page_kb) * 1024;
    bool truncated = false;
    if (cap > 0 && ext.text.size() > cap) {
        ext.text.resize(cap);
        ext.text += "\n[... page truncated at " + std::to_string(wc.max_web_page_kb) +
                    " KB ...]";
        truncated = true;
    }

    if (ext.text.empty())
        return error_result("Fetch succeeded but no readable text was extracted "
                            "from " + url);

    // Title: first h1 if any, else the domain.
    std::string title;
    for (const auto& h : ext.headings)
        if (h.level == 1) { title = h.text; break; }
    if (title.empty() && !ext.headings.empty()) title = ext.headings.front().text;
    if (title.empty()) title = extract_domain(url);

    // S6.0 injection scan + policy on the untrusted body (mutates ext.text).
    uint32_t flags = 0;
    std::string banner;
    scan_ingress(ws, ext.text, flags, banner);

    const std::string final_url = resp.final_url.empty() ? url : resp.final_url;
    int64_t page_id = cache->store_page(final_url, title, ext.text, ext.headings,
                                        /*session_id=*/"", flags);
    if (page_id < 0)
        return error_result("Failed to cache fetched page " + final_url);

    // Compact outline only (~30 tokens) -- never the full body.
    std::ostringstream out;
    out << "Fetched and indexed: " << final_url << "\n";
    out << "Title: " << (title.empty() ? "(untitled)" : title) << "\n";
    if (truncated) out << "(content truncated to " << wc.max_web_page_kb << " KB)\n";
    out << "Headings:\n";
    if (ext.headings.empty()) {
        out << "  (none)\n";
    } else {
        int shown = 0;
        for (const auto& h : ext.headings) {
            out << "  " << std::string(static_cast<size_t>((std::max)(0, h.level - 1)), ' ')
                << "- " << h.text << "\n";
            if (++shown >= 50) { out << "  ... (more headings)\n"; break; }
        }
    }
    out << "\nRead a section with web_read(url, heading=...) or search with "
           "search_text(query, sources=\"web\").\n";
    if (!banner.empty())
        out << "\n[!] " << banner << "\n";

    std::string body = out.str();
    ToolResult r{true, body, body};
    if (!banner.empty()) {
        r.activity_tag     = "injection_scan";
        r.activity_summary = banner;
        r.activity_detail  = "Untrusted web content from '" + extract_domain(final_url) +
                             "' was wrapped. " + banner;
    }
    return r;
}

// -- WebReadTool ------------------------------------------------------------

bool WebReadTool::available(IWorkspaceServices& ws) const { return web_available(ws); }

std::string WebReadTool::preview(const ToolCall& call) const
{
    std::string u = preview_text(tools::coerce_string(call.args, "url"), 80);
    std::string h = tools::coerce_string(call.args, "heading");
    std::string p = u.empty() ? std::string("web_read") : ("web_read: " + u);
    if (!h.empty()) p += " (#" + preview_text(h, 40) + ")";
    return p;
}

ToolResult WebReadTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call,
            {"url", "heading", "offset", "length"}, this))
        return *err;

    std::string url = tools::coerce_string(call.args, "url");
    if (url.empty())
        return tools::missing_required_arg(*this, "url", "URL of a fetched page");

    auto* cache = ws.web_cache();
    if (!cache) return error_result("Error: web cache not available");

    auto content_opt = cache->get_content(url);
    if (!content_opt)
        return error_result("Page not fetched. Call web_fetch(\"" + url +
                            "\") first.");
    const std::string& content = *content_opt;

    std::string heading = tools::coerce_string(call.args, "heading");
    std::string section;
    std::string header_line;

    if (!heading.empty()) {
        // Locate the heading text inside the stored plain text (case-insensitive
        // line match). Stored content carries headings on their own line (gumbo
        // walk emits a block newline after each h1-h6), so a line-prefix match
        // is robust. Section runs from that line to the next stored heading.
        std::vector<std::string> lines;
        {
            std::stringstream ss(content);
            std::string l;
            while (std::getline(ss, l)) lines.push_back(l);
        }
        auto headings = cache->get_headings(url);
        std::string want = lower(heading);
        int start_line = -1;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string ll = lower(lines[i]);
            // trim leading whitespace
            auto p = ll.find_first_not_of(" \t");
            if (p != std::string::npos) ll = ll.substr(p);
            if (ll.rfind(want, 0) == 0) { start_line = static_cast<int>(i); break; }
        }
        if (start_line < 0)
            return error_result("Heading \"" + heading + "\" not found in " + url +
                                ". Use web_read with offset/length, or check the "
                                "outline from web_fetch.");
        header_line = lines[start_line];
        // Build the set of all stored heading texts (lowercased, trimmed) so we
        // stop at the next one.
        std::vector<std::string> heading_texts;
        for (const auto& h : headings) heading_texts.push_back(lower(h.text));
        std::ostringstream sec;
        for (size_t i = static_cast<size_t>(start_line); i < lines.size(); ++i) {
            if (i != static_cast<size_t>(start_line)) {
                std::string ll = lower(lines[i]);
                auto p = ll.find_first_not_of(" \t");
                if (p != std::string::npos) ll = ll.substr(p);
                bool is_heading = false;
                for (const auto& ht : heading_texts)
                    if (!ht.empty() && ll.rfind(ht, 0) == 0) { is_heading = true; break; }
                if (is_heading) break;
            }
            sec << lines[i] << "\n";
        }
        section = sec.str();
    } else {
        // Line-window read.
        int offset = static_cast<int>(tools::coerce_int(call.args, "offset", 1));
        int length = static_cast<int>(tools::coerce_int(call.args, "length", 100));
        if (offset < 1) offset = 1;
        if (length < 1) length = 100;

        std::vector<std::string> lines;
        {
            std::stringstream ss(content);
            std::string l;
            while (std::getline(ss, l)) lines.push_back(l);
        }
        int total = static_cast<int>(lines.size());
        int start = offset - 1;
        if (start >= total)
            return error_result("offset " + std::to_string(offset) +
                                " is past the end (" + std::to_string(total) +
                                " lines) of " + url);
        int end = (std::min)(total, start + length);
        std::ostringstream sec;
        for (int i = start; i < end; ++i) sec << lines[i] << "\n";
        section = sec.str();
        header_line = url + " lines " + std::to_string(offset) + "-" +
                      std::to_string(end);
    }

    // Re-stamp the taint surface at the read site (S6.0): the model sees the
    // untrusted framing at the moment it reads, even if the stored body's own
    // nonce fence was trimmed by the section slice.
    uint32_t flags = cache->get_injection_flags(url);
    std::string rendered = security::render_tainted_snippet(section, "web", flags);

    std::ostringstream out;
    if (!heading.empty()) out << "Section: " << header_line << "\n";
    else                  out << header_line << "\n";
    out << rendered;

    std::string body = out.str();
    return {true, body, body};
}

} // namespace locus

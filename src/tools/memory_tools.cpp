#include "tools/memory_tools.h"
#include "tools/shared.h"

#include "core/memory_store.h"
#include "core/workspace.h"
#include "core/workspace_services.h"
#include "llm/token_counter.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

namespace locus {

using json = nlohmann::json;
using tools::error_result;

namespace {

// Schema lets the caller pass either an array of strings or a single string.
// Returns the canonicalised list (trimmed, empty entries dropped).
std::vector<std::string> parse_tag_arg(const json& v)
{
    std::vector<std::string> out;
    auto push = [&](std::string s) {
        // Trim
        auto a = s.find_first_not_of(" \t");
        if (a == std::string::npos) return;
        auto b = s.find_last_not_of(" \t");
        s = s.substr(a, b - a + 1);
        if (!s.empty()) out.push_back(std::move(s));
    };

    if (v.is_array()) {
        for (auto& e : v) {
            if (e.is_string()) push(e.get<std::string>());
        }
    } else if (v.is_string()) {
        // Allow comma-separated single string form: "build, conventions".
        std::stringstream ss(v.get<std::string>());
        std::string tok;
        while (std::getline(ss, tok, ',')) push(std::move(tok));
    }
    return out;
}

// Render a single memory hit for the LLM. Returns the chunk of text plus its
// estimated token cost so callers can enforce the response-size cap.
std::string render_hit(const MemoryStore::SearchHit& h, int rank)
{
    std::ostringstream o;
    o << "[" << rank << "] memory:" << h.entry.id;
    if (h.entry.pinned)         o << "  *pinned*";
    if (!h.entry.tags.empty()) {
        o << "  tags=";
        for (size_t i = 0; i < h.entry.tags.size(); ++i) {
            if (i) o << ",";
            o << h.entry.tags[i];
        }
    }
    o << "\n" << h.entry.content;
    if (h.entry.content.empty() || h.entry.content.back() != '\n') o << "\n";
    return o.str();
}

} // namespace

// -- AddMemoryTool ------------------------------------------------------------

bool AddMemoryTool::available(IWorkspaceServices& ws) const
{
    if (ws.memory() == nullptr) return false;
    // S5.A -- gate on capability bit so toggling memory_bank off in Settings
    // hides the tool on the next turn even if the MemoryStore was already
    // constructed when the workspace opened.
    auto* w = ws.workspace();
    return w ? w->config().capabilities.memory_bank : true;
}

std::string AddMemoryTool::preview(const ToolCall& call) const
{
    std::string c;
    if (call.args.contains("content") && call.args["content"].is_string())
        c = call.args["content"].get<std::string>();
    // Collapse any newlines so the chip-style bubble stays one line; truncate
    // the body so a multi-paragraph memory doesn't smother the chat.
    std::replace(c.begin(), c.end(), '\n', ' ');
    std::replace(c.begin(), c.end(), '\r', ' ');
    if (c.size() > 80) c = c.substr(0, 77) + "...";

    std::string out = "add_memory: " + (c.empty() ? "(empty)" : c);

    if (call.args.contains("tags")) {
        auto tags = parse_tag_arg(call.args["tags"]);
        if (!tags.empty()) out += "  tags=" + std::to_string(tags.size());
    }
    if (tools::coerce_bool(call.args, "pinned", false)) out += "  [pinned]";
    return out;
}

ToolResult AddMemoryTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                   const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call,
            {"content", "tags", "pinned", "source"}, this))
        return *err;

    auto* mem = ws.memory();
    if (!mem) return error_result("Error: memory bank is disabled for this workspace");

    std::string content;
    if (call.args.contains("content") && call.args["content"].is_string())
        content = call.args["content"].get<std::string>();
    if (content.empty())
        return tools::missing_required_arg(*this, "content",
            "the verbatim note text to save to the memory bank");

    std::vector<std::string> tags;
    if (call.args.contains("tags")) tags = parse_tag_arg(call.args["tags"]);

    bool pinned = tools::coerce_bool(call.args, "pinned", false);

    std::string source = "agent";
    if (call.args.contains("source") && call.args["source"].is_string()) {
        std::string s = call.args["source"].get<std::string>();
        if (s == "user" || s == "agent" || s == "mined") source = s;
    }

    std::string id;
    try {
        id = mem->add(content, tags, pinned, source);
    } catch (const std::exception& ex) {
        return error_result(std::string("Error: add_memory failed: ") + ex.what());
    }

    // Return the saved entry verbatim so the agent has a confirmation tail
    // and the activity-log sees what was committed.
    std::ostringstream out;
    out << "Saved memory:" << id;
    if (pinned) out << " (pinned)";
    if (!tags.empty()) {
        out << " tags=";
        for (size_t i = 0; i < tags.size(); ++i) {
            if (i) out << ",";
            out << tags[i];
        }
    }
    out << "\n" << content;
    if (content.back() != '\n') out << "\n";

    spdlog::info("add_memory: id={} pinned={} tags={} bytes={}",
                 id, pinned, tags.size(), content.size());

    std::string s = out.str();
    return { true, s, s };
}

// -- SearchMemoryTool ---------------------------------------------------------

bool SearchMemoryTool::available(IWorkspaceServices& ws) const
{
    if (ws.memory() == nullptr) return false;
    auto* w = ws.workspace();
    return w ? w->config().capabilities.memory_bank : true;
}

std::string SearchMemoryTool::preview(const ToolCall& call) const
{
    std::string q = call.args.value("query", "");
    if (q.size() > 80) q = q.substr(0, 77) + "...";
    std::string out = "search_memory";
    if (!q.empty()) out += ": " + q;
    if (call.args.contains("tags")) {
        auto tags = parse_tag_arg(call.args["tags"]);
        if (!tags.empty()) out += "  tags=" + std::to_string(tags.size());
    }
    return out;
}

ToolResult SearchMemoryTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                      const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call,
            {"query", "tags", "max_results"}, this))
        return *err;

    auto* mem = ws.memory();
    if (!mem) return error_result("Error: memory bank is disabled for this workspace");

    std::string query;
    if (call.args.contains("query") && call.args["query"].is_string())
        query = call.args["query"].get<std::string>();

    int max_results = 5;
    if (call.args.contains("max_results") && call.args["max_results"].is_number_integer())
        max_results = std::max(1, call.args["max_results"].get<int>());

    std::vector<std::string> tag_filter;
    if (call.args.contains("tags")) tag_filter = parse_tag_arg(call.args["tags"]);

    std::vector<MemoryStore::SearchHit> hits;
    try {
        hits = mem->search(query, max_results, tag_filter);
    } catch (const std::exception& ex) {
        return error_result(std::string("Error: search_memory failed: ") + ex.what());
    }

    // Cap the response by token budget (default 1500 tokens). The cap acts on
    // the rendered output: we stop appending once another hit would push the
    // running token estimate past the limit.
    int cap_tokens = 1500;
    if (auto* wsp = ws.workspace()) {
        // No direct WorkspaceConfig accessor here -- rely on the default
        // unless the workspace exposes one. The default matches WorkspaceConfig
        // .memory.search_response_max_tokens (1500). A future revision can
        // wire IWorkspaceServices::config() through if a per-call override
        // becomes necessary.
        (void)wsp;
    }

    std::ostringstream out;
    if (hits.empty()) {
        std::string msg = "No memory entries matched";
        if (!tag_filter.empty()) {
            msg += " (tag filter applied)";
        }
        msg += ".";
        return { true, msg, msg };
    }

    out << hits.size() << " memory match" << (hits.size() == 1 ? "" : "es");
    if (!tag_filter.empty()) {
        out << " (filtered by tag";
        if (tag_filter.size() > 1) out << "s";
        out << ")";
    }
    out << ":\n\n";

    int running = TokenCounter::estimate(out.str());
    bool truncated = false;
    int rank = 0;
    for (auto& h : hits) {
        ++rank;
        std::string rendered = render_hit(h, rank);
        int cost = TokenCounter::estimate(rendered);
        if (running + cost > cap_tokens && rank > 1) {
            truncated = true;
            break;
        }
        out << rendered << "\n";
        running += cost;
    }
    if (truncated) {
        out << "[response truncated at " << cap_tokens
            << " tokens; refine your query for more]";
    }

    spdlog::info("search_memory: query='{}' hits={} tag_filter={}",
                 query, hits.size(), tag_filter.size());

    std::string s = out.str();
    return { true, s, s };
}

} // namespace locus

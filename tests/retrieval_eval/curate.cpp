#include "curate.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <set>
#include <sstream>

namespace locus::eval {

using json = nlohmann::json;

namespace {

// Parse a tool-call's `arguments` (always a raw JSON string in OpenAI-compatible
// schema) and append every search query found to `out`. The unified `search`
// tool encodes mode as a field; the legacy per-mode tools (search_text /
// search_semantic / ...) encode mode in the tool name.
void extract_from_tool_call(const std::string& tool_name,
                            const std::string& raw_args,
                            const std::string& session_id,
                            std::vector<CuratedQuery>& out)
{
    json args;
    try {
        args = json::parse(raw_args);
    } catch (const json::exception&) {
        return;  // malformed args — model hallucination, skip silently
    }
    if (!args.is_object()) return;

    std::string mode;
    if (tool_name == "search") {
        mode = args.value("mode", "");
    } else if (tool_name.rfind("search_", 0) == 0) {
        mode = tool_name.substr(7);  // legacy: search_text -> "text"
    } else {
        return;  // not a search tool
    }
    if (mode.empty()) return;

    CuratedQuery q;
    q.source_session = session_id;
    q.mode           = mode;
    q.query          = args.value("query", "");
    q.pattern        = args.value("pattern", "");

    // Skip empties — happens when the model called a search tool with the
    // wrong arg shape (regex without `pattern`, semantic without `query`, ...).
    if (q.query.empty() && q.pattern.empty()) return;
    out.push_back(std::move(q));
}

} // namespace

std::vector<CuratedQuery>
curate_from_sessions(const fs::path& sessions_dir)
{
    std::vector<CuratedQuery> out;

    std::error_code ec;
    if (!fs::exists(sessions_dir, ec) || !fs::is_directory(sessions_dir, ec)) {
        spdlog::info("Curate: sessions dir not present ({}); nothing to extract",
                     sessions_dir.string());
        return out;
    }

    int session_count = 0;
    for (auto& entry : fs::directory_iterator(sessions_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::string session_id = entry.path().stem().string();
        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        json j;
        try {
            j = json::parse(f);
        } catch (const json::exception& e) {
            spdlog::warn("Curate: skipping malformed session {}: {}",
                         session_id, e.what());
            continue;
        }

        if (!j.contains("messages") || !j["messages"].is_array()) continue;

        for (auto& msg : j["messages"]) {
            if (!msg.contains("tool_calls")) continue;
            if (!msg["tool_calls"].is_array()) continue;
            for (auto& tc : msg["tool_calls"]) {
                if (!tc.is_object() || !tc.contains("function")) continue;
                auto& fn = tc["function"];
                std::string name = fn.value("name", "");
                std::string args = fn.value("arguments", "");
                if (name.empty() || args.empty()) continue;
                extract_from_tool_call(name, args, session_id, out);
            }
        }
        ++session_count;
    }

    spdlog::info("Curate: extracted {} search queries from {} session file(s)",
                 out.size(), session_count);
    return out;
}

std::string to_queries_json_template(const std::vector<CuratedQuery>& queries)
{
    // Deduplicate on (mode, normalised query) — sessions often repeat the same
    // search verbatim across turns.
    std::set<std::pair<std::string, std::string>> seen;

    json arr = json::array();
    for (const auto& q : queries) {
        std::string key_text = !q.query.empty() ? q.query : q.pattern;
        auto key = std::make_pair(q.mode, key_text);
        if (seen.count(key)) continue;
        seen.insert(key);

        json entry;
        entry["query"] = key_text;
        entry["expected_files"] = json::array();
        entry["_curated_from"] = q.source_session;
        entry["_mode"]         = q.mode;
        arr.push_back(std::move(entry));
    }

    json doc;
    doc["_about"] = "Curated search queries from saved sessions. "
                    "Hand-fill each `expected_files` array, drop the `_*` "
                    "metadata fields, then merge into queries.json.";
    doc["queries"] = std::move(arr);
    return doc.dump(2);
}

} // namespace locus::eval

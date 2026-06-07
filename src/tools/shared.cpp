#include "tools/shared.h"

#include "core/workspace_services.h"
#include "llm/json_repair.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  include <cwctype>
#endif

namespace locus::tools {

namespace fs = std::filesystem;

bool coerce_bool(const nlohmann::json& args, const char* key, bool fallback)
{
    auto it = args.find(key);
    if (it == args.end() || it->is_null())
        return fallback;
    const nlohmann::json& v = *it;
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number_integer())
        return v.get<long long>() != 0;
    if (v.is_number_float())
        return v.get<double>() != 0.0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (s == "true" || s == "1" || s == "yes" || s == "on")
            return true;
        if (s == "false" || s == "0" || s == "no" || s == "off" || s.empty())
            return false;
    }
    return fallback;
}

long long coerce_int(const nlohmann::json& args, const char* key, long long fallback)
{
    auto it = args.find(key);
    if (it == args.end() || it->is_null())
        return fallback;
    const nlohmann::json& v = *it;
    if (v.is_number_integer())  return v.get<long long>();
    if (v.is_number_unsigned()) return static_cast<long long>(v.get<unsigned long long>());
    if (v.is_number_float())    return static_cast<long long>(v.get<double>());
    if (v.is_boolean())         return v.get<bool>() ? 1 : 0;
    if (v.is_string()) {
        const std::string& s = v.get<std::string>();
        try {
            size_t pos = 0;
            long long n = std::stoll(s, &pos);
            // Accept "100", "100.5" (stoll stops at '.'), " 100 " -> tolerate
            // trailing junk so a model's "100px"-style slip still parses.
            if (pos > 0) return n;
        } catch (...) { /* not a number -> fallback */ }
    }
    return fallback;
}

nlohmann::json coerce_json_array(const nlohmann::json& args, const char* key)
{
    auto it = args.find(key);
    if (it == args.end() || it->is_null())
        return nlohmann::json::array();
    if (it->is_array())
        return *it;
    if (it->is_string()) {
        // The model double-encoded the array as a JSON string. Parse it.
        const std::string raw = it->get<std::string>();
        auto parsed = nlohmann::json::parse(raw,
                                            /*cb=*/nullptr,
                                            /*allow_exceptions=*/false);
        if (parsed.is_array())
            return parsed;
        // Small / hosted models sometimes emit a Python-dict-style body inside
        // that string (single-quoted keys: "[{'description': ...}]") which is
        // not valid JSON and fails the strict parse above. Run it through the
        // same json-repair pre-pass the tool-call parser uses (single-quote ->
        // double-quote, trailing-comma fixups, etc.) and retry. Mirrors the
        // S6.19 robustness contract for edit_file's `edits` arg. repair_for_parse
        // returns text guaranteed to parse, but it may parse to a non-array
        // (e.g. an object) -- so we still gate on is_array().
        if (auto repaired = locus::repair_for_parse(raw)) {
            auto reparsed = nlohmann::json::parse(repaired->fixed,
                                                  /*cb=*/nullptr,
                                                  /*allow_exceptions=*/false);
            if (reparsed.is_array())
                return reparsed;
        }
    }
    return nlohmann::json::array();
}

namespace {

// Case-insensitive on Windows, case-sensitive elsewhere. Matches NTFS default
// and the rest of the filesystem layer's effective behaviour. UNC and drive
// letters are part of the first component so they're covered.
bool components_equal(const fs::path& a, const fs::path& b)
{
#if defined(_WIN32)
    const auto& sa = a.native();
    const auto& sb = b.native();
    if (sa.size() != sb.size()) return false;
    for (size_t i = 0; i < sa.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(sa[i]))
            != std::towlower(static_cast<wint_t>(sb[i])))
            return false;
    }
    return true;
#else
    return a.native() == b.native();
#endif
}

} // namespace

bool is_inside(const fs::path& candidate, const fs::path& root)
{
    fs::path c = candidate.lexically_normal();
    fs::path r = root.lexically_normal();

    // Drop trailing empty component that lexically_normal leaves on paths
    // ending in a separator ("D:/foo/" -> components {"D:", "/", "foo", ""}).
    auto strip_trailing_empty = [](fs::path& p) {
        if (!p.empty() && p.filename().empty())
            p = p.parent_path();
    };
    strip_trailing_empty(c);
    strip_trailing_empty(r);

    auto cit = c.begin();
    auto cend = c.end();
    auto rit = r.begin();
    auto rend = r.end();

    while (rit != rend) {
        if (cit == cend) return false;          // root deeper than candidate
        if (!components_equal(*rit, *cit)) return false;
        ++rit;
        ++cit;
    }
    // Either equal paths (cit == cend) or candidate is strictly deeper.
    return true;
}

fs::path resolve_path(IWorkspaceServices& ws, const std::string& rel)
{
    std::error_code ec;
    fs::path full = fs::canonical(ws.root() / rel, ec);
    if (ec) {
        // canonical failed (file may not exist yet) -- try weakly_canonical
        full = fs::weakly_canonical(ws.root() / rel, ec);
        if (ec) return {};
    }

    fs::path root = fs::canonical(ws.root(), ec);
    if (ec) root = ws.root();

    if (!is_inside(full, root))
        return {};   // path traversal attempt

    return full;
}

std::string write_atomic(const fs::path& target, const std::string& content)
{
    fs::path tmp = target;
    tmp += ".locus-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return "cannot open temp file for write: " + tmp.string();
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out.good())
            return "write failed on temp file: " + tmp.string();
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        // Fallback: rename can fail cross-device / with readers holding handles.
        // Copy + remove tmp is the safety net.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) return "rename/copy to target failed: " + ec.message();
    }
    return {};
}

std::string make_relative(IWorkspaceServices& ws, const fs::path& p)
{
    return fs::relative(p, ws.root()).string();
}

ToolResult error_result(const std::string& msg)
{
    return {false, msg, msg};
}

// S6.17 Task D -- per-tool alias map. Keyed by (tool_name, unknown_key); value
// is a one-sentence suggestion that names the canonical key(s) the model
// should have used. Lifts the eight or so silent-fallback bugs observed in
// TestLocalVibe2 into explicit errors with a recovery path.
namespace {

struct AliasEntry {
    std::string_view tool;
    std::string_view key;
    std::string_view suggest;
};

// Suggestion text used for `type` / `kind` / `search_mode` keys sent against
// a per-mode search_* tool (Task G split: the unified `search` face is gone,
// so a `mode` arg is meaningless -- the model picks the right tool by name).
constexpr const char* k_search_mode_hint =
    "the unified 'search' tool was split (S6.17 Task G) -- pick by name: "
    "search_text / search_regex / search_symbols / search_semantic / "
    "search_ast";

constexpr std::array<AliasEntry, 29> k_aliases = {{
    // read_file (TestLocalVibe2 Pass 2/5: lines:"100-152" silently became
    // offset=1/length=100; agent retried 6 times before working around it).
    {"read_file",   "lines",       "use 'offset' (1-based) + 'length' instead, e.g. {\"path\":\"...\",\"offset\":100,\"length\":53}"},
    {"read_file",   "line_range",  "use 'offset' + 'length' instead"},
    {"read_file",   "range",       "use 'offset' + 'length' instead"},
    {"read_file",   "start",       "use 'offset' + 'length' instead"},
    {"read_file",   "end",         "use 'offset' + 'length' instead"},

    // Per-mode search_* tools: a model that sent `type` / `kind` / `search_mode`
    // was reaching for the retired discriminator. Aliases on every per-mode
    // tool point the model at the per-tool naming. `kind` is a real arg on
    // search_symbols (kind=function / class / etc.) so it is NOT aliased there.
    {"search_text",     "type",        k_search_mode_hint},
    {"search_text",     "kind",        k_search_mode_hint},
    {"search_text",     "search_mode", k_search_mode_hint},
    {"search_text",     "mode",        k_search_mode_hint},
    {"search_regex",    "type",        k_search_mode_hint},
    {"search_regex",    "kind",        k_search_mode_hint},
    {"search_regex",    "search_mode", k_search_mode_hint},
    {"search_regex",    "mode",        k_search_mode_hint},
    {"search_symbols",  "type",        k_search_mode_hint},
    {"search_symbols",  "search_mode", k_search_mode_hint},
    {"search_symbols",  "mode",        k_search_mode_hint},
    {"search_semantic", "type",        k_search_mode_hint},
    {"search_semantic", "kind",        k_search_mode_hint},
    {"search_semantic", "search_mode", k_search_mode_hint},
    {"search_semantic", "mode",        k_search_mode_hint},

    // run_command / run_command_bg (TestLocalVibe2 Pass 5 hit `cmd:` first).
    {"run_command",     "cmd",   "use 'command' (the shell command line)"},
    {"run_command",     "shell", "use 'command' (the shell command line)"},
    {"run_command",     "exec",  "use 'command' (the shell command line)"},
    {"run_command_bg",  "cmd",   "use 'command' (the shell command line)"},
    {"run_command_bg",  "shell", "use 'command' (the shell command line)"},
    {"run_command_bg",  "exec",  "use 'command' (the shell command line)"},

    // write_file body shape.
    {"write_file",  "body",     "use 'content' (full file body as one string)"},
    {"write_file",  "text",     "use 'content' (full file body as one string)"},
    {"write_file",  "data",     "use 'content' (full file body as one string)"},
}};

std::optional<std::string> suggest_for(const std::string& tool,
                                       const std::string& unknown_key)
{
    for (const auto& a : k_aliases) {
        if (a.tool == tool && a.key == unknown_key)
            return std::string(a.suggest);
    }
    return std::nullopt;
}

} // namespace

std::string render_tool_schema(const ITool& tool)
{
    nlohmann::json schema = nlohmann::json::object();
    schema["name"]        = tool.name();
    schema["description"] = tool.description();

    // Prefer the tool's full JSON Schema if it supplied one (MCP path).
    // Otherwise project params() into a minimal {properties, required} body.
    nlohmann::json raw = tool.parameters_schema();
    if (raw.is_object() && !raw.empty()) {
        if (!raw.contains("type")) raw["type"] = "object";
        schema["parameters"] = std::move(raw);
    } else {
        nlohmann::json props = nlohmann::json::object();
        nlohmann::json required_list = nlohmann::json::array();
        for (auto& p : tool.params()) {
            nlohmann::json prop;
            prop["type"]        = p.type;
            prop["description"] = p.description;
            props[p.name]       = prop;
            if (p.required) required_list.push_back(p.name);
        }
        nlohmann::json params_schema = nlohmann::json::object();
        params_schema["type"]       = "object";
        params_schema["properties"] = std::move(props);
        if (!required_list.empty())
            params_schema["required"] = std::move(required_list);
        schema["parameters"] = std::move(params_schema);
    }
    return schema.dump(2);
}

ToolResult error_with_schema(const std::string& message, const ITool& tool)
{
    std::string body = message;
    body += "\n\nTool schema for retry (use these arg names + types):\n";
    body += render_tool_schema(tool);
    return error_result(body);
}

ToolResult missing_required_arg(const ITool& tool,
                                std::string_view arg_name,
                                std::string_view detail)
{
    std::string msg = "Error: missing required argument '";
    msg.append(arg_name);
    msg += "' for tool '";
    msg += tool.name();
    msg += "'";
    if (!detail.empty()) {
        msg += " (";
        msg.append(detail);
        msg += ")";
    }
    msg += ".";
    return error_with_schema(msg, tool);
}

std::optional<ToolResult> reject_unknown_keys(
    const ToolCall& call,
    std::initializer_list<const char*> allowed,
    const ITool* tool)
{
    if (!call.args.is_object()) return std::nullopt;
    for (auto it = call.args.begin(); it != call.args.end(); ++it) {
        const std::string& key = it.key();
        bool ok = false;
        for (const char* a : allowed) {
            if (key == a) { ok = true; break; }
        }
        if (ok) continue;

        std::string msg = "Error: unrecognized argument '" + key + "' for tool '"
                          + call.tool_name + "'";
        if (auto s = suggest_for(call.tool_name, key))
            msg += "; " + *s + ".";
        else
            msg += ".";

        if (tool != nullptr)
            return error_with_schema(msg, *tool);

        // Legacy path: no tool pointer -> point the model at describe_tool.
        msg += " Call describe_tool('" + call.tool_name + "') for the schema.";
        return error_result(msg);
    }
    return std::nullopt;
}

// S6.10 Task B -- shared closest-name helper. Levenshtein bounded by the
// number of registered tools (~20); cheap enough that we don't need a max
// distance cutoff. Returns empty when `candidates` is empty.
namespace {
int levenshtein_local(const std::string& a, const std::string& b)
{
    const std::size_t m = a.size();
    const std::size_t n = b.size();
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (std::size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<int>(i);
    for (std::size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= m; ++i) {
        for (std::size_t j = 1; j <= n; ++j) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1,
                                 dp[i][j - 1] + 1,
                                 dp[i - 1][j - 1] + cost});
        }
    }
    return dp[m][n];
}
} // namespace

std::string closest_tool_name(const std::string& target,
                              const std::vector<ITool*>& candidates)
{
    std::string best;
    int best_d = std::numeric_limits<int>::max();
    for (auto* t : candidates) {
        if (!t) continue;
        int d = levenshtein_local(target, t->name());
        if (d < best_d) { best_d = d; best = t->name(); }
    }
    return best;
}

ToolApprovalPolicy resolve_approval_policy(
    const std::string& tool_name,
    ToolApprovalPolicy tool_default,
    const std::unordered_map<std::string, ToolApprovalPolicy>& overrides)
{
    auto exact = overrides.find(tool_name);
    if (exact != overrides.end()) return exact->second;

    for (const auto& [key, val] : overrides) {
        // Match keys ending in ":*". The matched prefix is everything up
        // to (and including) the trailing ':'; the tool name must be
        // strictly longer than the prefix so "mcp:fs:*" doesn't match the
        // (impossible but well-defined) "mcp:fs:".
        if (key.size() < 2) continue;
        if (key.compare(key.size() - 2, 2, ":*") != 0) continue;
        std::string prefix(key.data(), key.size() - 1);  // drop the '*', keep ':'
        if (tool_name.size() > prefix.size() &&
            tool_name.compare(0, prefix.size(), prefix) == 0) {
            return val;
        }
    }
    return tool_default;
}

// -- ReadTracker ------------------------------------------------------------

ReadTracker& ReadTracker::instance()
{
    static ReadTracker s_tracker;
    return s_tracker;
}

void ReadTracker::mark_read(const fs::path& canonical_path)
{
    std::lock_guard lock(mu_);
    seen_.insert(canonical_path.string());
}

bool ReadTracker::was_read(const fs::path& canonical_path) const
{
    std::lock_guard lock(mu_);
    return seen_.count(canonical_path.string()) > 0;
}

void ReadTracker::clear()
{
    std::lock_guard lock(mu_);
    seen_.clear();
}

} // namespace locus::tools

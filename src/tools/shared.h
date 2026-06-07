#pragma once

#include "tool.h"

#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace locus {
class IWorkspaceServices;
}

namespace locus::tools {

// S6.17 Task D -- reject calls that carry unknown argument keys. Without this
// helper, tools silently fell back to defaults (read_file with `lines:` ->
// offset=1/length=100, search with `type:` -> mode=text, etc.), which broke
// small-model agentic workflows because the model had no signal to fix its
// arg shape. Pass the explicit allow-list; helper returns an error_result
// when any key sits outside the list, with a suggestion when the key matches
// a known alias (lines / type / cmd / body / ...).
//
// Schema-on-error: when `tool` is non-null, the canonical schema (built from
// `tool->params()` or `tool->parameters_schema()`) is appended to the error
// content so the model can self-correct on the next round without burning a
// separate `describe_tool` call. This is the small-model safety net for
// lazy_tool_manifest -- the model gets the same JSON shape `describe_tool`
// would have returned, delivered proactively.
//
// Usage at the top of execute():
//   if (auto err = reject_unknown_keys(call, {"path", "offset", "length"}, this))
//       return *err;
//
// Note: tool dispatcher already lifts wrappers (`arguments`, `parameters`,
// `args`, `input`) so they never reach this helper.
std::optional<ToolResult> reject_unknown_keys(
    const ToolCall& call,
    std::initializer_list<const char*> allowed,
    const ITool* tool = nullptr);

// Render the canonical schema for an error response. Returns a JSON-formatted
// string in the same shape `describe_tool` produces (description +
// parameters). Used by `reject_unknown_keys` / `missing_required_arg` /
// `error_with_schema` to append schema to error messages so the model can
// self-correct on the next round without a separate fetch.
std::string render_tool_schema(const ITool& tool);

// Build an error_result with `message` followed by the tool's schema. Use
// this for arg-shape errors discovered after `reject_unknown_keys` -- e.g.
// type mismatches, empty arrays, mutually-exclusive args. Semantic errors
// (file not found, search returned zero results) should keep using bare
// `error_result(msg)` since the schema doesn't help diagnose them.
ToolResult error_with_schema(const std::string& message, const ITool& tool);

// Canonical "missing required argument" error. Wraps the standard message
// plus the schema in one call. `arg_name` is the canonical name as it
// appears in `tool.params()` -- legacy aliases should be mentioned in
// `detail` if you want to nudge the model toward the new name.
ToolResult missing_required_arg(const ITool& tool,
                                std::string_view arg_name,
                                std::string_view detail = "");

// Resolve the effective approval policy for `tool_name`, given the tool's
// built-in default and the per-workspace override map.
//
// Lookup order (first hit wins):
//   1. Exact override match by tool name.
//   2. Prefix override with a ":*" suffix (e.g. "mcp:fs:*" matches
//      "mcp:fs:read_file"). Multiple prefix entries are checked in iteration
//      order -- callers shouldn't define overlapping prefixes.
//   3. Tool's built-in default (`tool_default`).
//
// A bare "*" wildcard is intentionally NOT supported: blanket auto-approve
// for everything would defeat the entire approval gate; if a user wants it
// they have to set it per built-in tool. The ":*" form is the
// per-MCP-server trust toggle added in S4.G phase 2.
ToolApprovalPolicy resolve_approval_policy(
    const std::string& tool_name,
    ToolApprovalPolicy tool_default,
    const std::unordered_map<std::string, ToolApprovalPolicy>& overrides);


// Type-tolerant boolean read from an LLM-supplied tool-arg object. Small /
// hosted models routinely emit booleans as the WRONG JSON type -- the string
// "true" / "True" / "1", or the integer 1 / 0 -- instead of a JSON bool.
// nlohmann's `json::value<bool>(key, default)` and `.get<bool>()` THROW a
// type_error on any non-bool value; when that throw lands on a UI-thread call
// site (e.g. WriteFileTool::preview during the tool-pending broadcast) there is
// no catch and the process fail-fasts (0xc0000409). This helper never throws:
// it accepts a real JSON bool, the numbers 0/1, and the strings
// true/false/1/0/yes/no/on/off (case-insensitive); anything else -> `fallback`.
// Use it at EVERY tool-arg boolean site reachable from model output.
bool coerce_bool(const nlohmann::json& args, const char* key, bool fallback);

// Type-tolerant array read from an LLM-supplied tool-arg object. Same failure
// mode as coerce_bool, one level up: small / hosted models routinely emit an
// array arg as a STRING containing the JSON array text -- e.g.
// `"edits":"[{\"old_string\":...}]"` instead of `"edits":[{...}]`. Reading it
// with `json::value(key, json::array())` THROWS type_error (default is array,
// stored is string), and on the UI-thread preview() path that throw fail-fasts
// the process (0xc0000409). This helper never throws:
//   - key absent / null            -> empty array
//   - value already an array       -> returned as-is
//   - value is a string that JSON-parses to an array -> the parsed array
//   - anything else                -> empty array
// Returns the parsed/array value. Pass the first present key; callers that
// accept aliases call it once per alias and take the first non-empty result.
nlohmann::json coerce_json_array(const nlohmann::json& args, const char* key);

// Type-tolerant integer read from an LLM-supplied tool-arg object. Same class
// as coerce_bool / coerce_json_array: models frequently emit a number as a
// STRING ("100") or as a float (100.0). `json::value<int>(key, default)` and
// `.get<int>()` THROW type_error on a string. In execute() that throw is caught
// by the dispatcher and surfaces as a confusing "internal error" tool result
// (wastes a round; the model can't tell it just mistyped the arg); in preview()
// it would crash. This helper never throws:
//   - key absent / null         -> fallback
//   - integer                   -> the value (clamped to long long range)
//   - float                     -> truncated to integer
//   - string parseable as a number (leading int, optional sign) -> that number
//   - anything else             -> fallback
long long coerce_int(const nlohmann::json& args, const char* key, long long fallback);

// Resolve a relative path safely within the workspace root.
// Returns empty path if the resolved path escapes the workspace.
std::filesystem::path resolve_path(IWorkspaceServices& ws, const std::string& rel);

std::string make_relative(IWorkspaceServices& ws, const std::filesystem::path& p);

// Component-aware workspace containment check (S5.O). True iff `candidate`
// equals `root` or sits strictly inside it; component-walk avoids the
// "/foo" vs "/foo-bar" prefix false positive that a raw string compare
// produces. On Windows the comparison is case-insensitive (NTFS default);
// elsewhere it's exact. Both paths are lexically_normal'd first so trailing
// slashes and "." segments don't shift the component count.
bool is_inside(const std::filesystem::path& candidate,
               const std::filesystem::path& root);

// Atomic write: write to <target>.locus-tmp then rename over the target.
// Keeps the file intact if the process is killed mid-write. Falls back to
// copy_file when rename fails (different volume, reader holding a handle).
// Returns an empty string on success, an error message otherwise.
std::string write_atomic(const std::filesystem::path& target,
                         const std::string& content);

ToolResult error_result(const std::string& msg);

// S6.10 Task B -- closest-name suggestion across a list of tools. Returns
// the candidate's `name()` with the smallest Levenshtein distance to
// `target`. Empty when `candidates` is empty. Promoted from describe_tool.cpp
// so ToolDispatcher's unknown-tool branch can share the helper.
std::string closest_tool_name(const std::string& target,
                              const std::vector<ITool*>& candidates);

// Process-wide "this file was read by the agent" tracker (S4.A).
// Edit tools require the file to have been read first -- mirrors the Claude
// Code pattern that cuts hallucinated edits on local models. The set only
// grows; canonical paths mean workspace switches don't collide.
class ReadTracker {
public:
    static ReadTracker& instance();

    void mark_read(const std::filesystem::path& canonical_path);
    bool was_read(const std::filesystem::path& canonical_path) const;
    void clear();  // tests

private:
    mutable std::mutex              mu_;
    std::unordered_set<std::string> seen_;
};

} // namespace locus::tools

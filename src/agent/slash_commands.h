#pragma once

#include "core/workspace_services.h"
#include "tool.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace locus {

// Raised by SlashCommandParser on malformed input: unterminated quote,
// unparseable integer/boolean argument, etc. Caller (Dispatcher) turns this
// into a user-visible error line; it never escapes the agent thread.
class SlashParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Result of parsing a slash command. `is_help == true` for `/help` — in that
// case tool_name/args are unused. For any other `/<name>`, tool_name is the
// identifier after the slash; the tool itself may or may not exist (the
// dispatcher handles the unknown case).
struct ParsedSlashCall {
    std::string    tool_name;
    nlohmann::json args = nlohmann::json::object();
    bool           is_help = false;
};

// Completion suggestion used by GUI autocomplete and `/help`.
struct SlashCompletion {
    std::string name;         // tool name (no leading '/')
    std::string description;  // short hint shown after the name
    std::string signature;    // e.g. "<path> [offset=integer] [limit=integer]"
};

// Pure tokenizer + arg-mapper. Shell-style quoted tokens, key=value pairs,
// positional fallback, type coercion driven by the tool's param schema.
// No execution, no output, no state.
class SlashCommandParser {
public:
    // Returns nullopt if `input` is not a slash command (no leading '/').
    // Throws SlashParseError on malformed input.
    static std::optional<ParsedSlashCall> parse(std::string_view input,
                                                const IToolRegistry& tools);
};

// Owns execution + output formatting. Stateless aside from its two refs.
class SlashCommandDispatcher {
public:
    SlashCommandDispatcher(IToolRegistry& tools, IWorkspaceServices& services);

    // Returns true if `input` was a slash command (handled or rejected).
    // `on_output` receives display text (markdown). `on_error` receives
    // user-visible error lines.
    bool try_dispatch(std::string_view input,
                      const std::function<void(std::string)>& on_output,
                      const std::function<void(std::string)>& on_error);

    // Returns tool completions whose name starts with `prefix` (prefix matches
    // first), then tools whose name contains `prefix` (case-insensitive).
    // Empty prefix returns every tool in registry order.
    std::vector<SlashCompletion> complete(std::string_view prefix) const;

private:
    std::string render_help() const;
    std::string format_result(const std::string& tool_name,
                              const ToolResult& result,
                              long long ms) const;

    IToolRegistry&      tools_;
    IWorkspaceServices& services_;
};

} // namespace locus

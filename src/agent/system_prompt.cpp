#include "system_prompt.h"
#include "llm/token_counter.h"

#include <spdlog/spdlog.h>

#include <sstream>

namespace locus {

namespace {

// Format-specific addendum injected when the configured wire format is
// not pure OpenAI JSON. Most LM Studio templates handle this for the
// model, but explicit instructions are a cheap belt-and-braces hedge
// against a broken or missing chat template.
const char* tool_format_instructions(ToolFormat f)
{
    switch (f) {
    case ToolFormat::Qwen:
        return "\n## Tool-call format\n"
               "When you call a tool, emit it on its own line as:\n"
               "<tool_call>\n"
               "{\"name\": \"<tool_name>\", \"arguments\": {<args object>}}\n"
               "</tool_call>\n"
               "Do not include any other text on the lines containing the markers. "
               "Use exactly one tool call per <tool_call> block.\n";
    case ToolFormat::Claude:
        return "\n## Tool-call format\n"
               "When you call a tool, emit it inside a <function_calls> block:\n"
               "<function_calls>\n"
               "<invoke name=\"<tool_name>\">\n"
               "<parameter name=\"<arg_name>\"><arg_value></parameter>\n"
               "</invoke>\n"
               "</function_calls>\n"
               "One <invoke> per tool. Multiple <parameter> blocks per invoke "
               "for multiple arguments. Values may be raw text, JSON arrays, "
               "or JSON objects.\n";
    case ToolFormat::None:
        return "\n## Tools\n"
               "No tools are available. Answer from your own knowledge "
               "of the workspace context provided above.\n";
    case ToolFormat::OpenAi:
    case ToolFormat::Auto:
    default:
        return "";
    }
}

} // namespace

std::string SystemPromptBuilder::build(const std::string& locus_md,
                                       const WorkspaceMetadata& meta,
                                       const IToolRegistry& tools,
                                       ToolFormat tool_format)
{
    std::ostringstream ss;

    // -- Base instructions ----------------------------------------------------
    ss << "You are Locus, a workspace-bound AI assistant. You help users understand, "
          "navigate, and modify files within a specific workspace folder.\n\n"
          "## Rules\n"
          "- Use tools to read files and search the index. Never guess file contents.\n"
          "- Paginate file reads — request chunks, never entire large files.\n"
          "- Summarize tool results concisely before responding.\n"
          "- When modifying files, show the user what you plan to do before executing.\n"
          "- Do not execute destructive operations (delete, overwrite) without explicit confirmation.\n"
          "- Be concise. Do not repeat information the user already has.\n\n"
          "## Editing and creating files\n"
          "- Prefer `edit_file` over `write_file` for changes to existing files. It is faster, cheaper, and cannot "
          "truncate or corrupt the file. `edit_file` takes an `edits` array — pass one element for a single change, "
          "or several for an atomic batch (all succeed or none are written; later edits see the results of earlier edits).\n"
          "- Use `write_file` to create new files (and for full rewrites, which require `overwrite=true`). "
          "By default `write_file` refuses to replace an existing file -- set `overwrite=true` only when you truly "
          "intend to replace the whole content. `write_file` also creates any missing parent directories on the "
          "path; there is no separate directory-creation tool.\n"
          "- Before calling `edit_file`, call `read_file` on the same path in this session — "
          "the editor refuses edits on files it has not seen to prevent hallucinated changes.\n"
          "- Each `old_string` must match byte-for-byte (including indentation and trailing whitespace) and must be "
          "unique in the file; widen the match with surrounding context if it is not, or set `replace_all=true`.\n\n"
          "## Running shell commands\n"
          "- Use `run_command` for short, synchronous commands (under ~30 seconds): build steps, scripts, queries.\n"
          "- Use `run_command_bg` for dev servers, file watchers, long builds, or anything that streams output. "
          "It returns a `process_id` that survives across turns. Read its output with `read_process_output` "
          "(omit `since_offset` to get only what's new since your last read), terminate with `stop_process`, "
          "and enumerate live processes with `list_processes`.\n\n";

    // -- Workspace metadata ---------------------------------------------------
    ss << "## Workspace\n"
       << "- Root: " << meta.root.string() << "\n"
       << "- Indexed files: " << meta.file_count << "\n"
       << "- Code symbols: " << meta.symbol_count << "\n"
       << "- Headings: " << meta.heading_count << "\n\n";

    // -- LOCUS.md (user-provided guide) ---------------------------------------
    if (!locus_md.empty()) {
        check_locus_md_budget(locus_md);
        ss << "## LOCUS.md (workspace guide provided by the user)\n\n"
           << locus_md << "\n\n";
    }

    // -- Available tools (text description) -----------------------------------
    ss << "## Available Tools\n\n";
    for (auto* tool : tools.all()) {
        ss << "- **" << tool->name() << "**: " << tool->description() << "\n";
        for (auto& p : tool->params()) {
            ss << "  - `" << p.name << "` (" << p.type;
            if (!p.required) ss << ", optional";
            ss << "): " << p.description << "\n";
        }
    }

    // -- Tool-call wire format addendum (S4.N) --------------------------------
    ss << tool_format_instructions(tool_format);

    return ss.str();
}

int SystemPromptBuilder::check_locus_md_budget(const std::string& locus_md)
{
    int tokens = TokenCounter::estimate(locus_md);
    if (tokens > 500) {
        spdlog::warn("LOCUS.md is ~{} tokens (budget: 500). Consider trimming it "
                     "to leave more context for conversation.", tokens);
    } else {
        spdlog::trace("LOCUS.md token estimate: {}", tokens);
    }
    return tokens;
}

} // namespace locus

#include "system_prompt_assembly.h"

#include "llm/token_counter.h"

#include <spdlog/spdlog.h>

#include <functional>
#include <sstream>

namespace locus {

namespace {

// Format-specific addendum (mirrors the helper inside system_prompt.cpp --
// kept private to each TU so the body is identical without a header churn
// in S5.J). Any future change to either copy must update both.
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

SystemPromptAssembly SystemPromptAssembly::build(const std::string&       locus_md,
                                                 const WorkspaceMetadata& meta,
                                                 const IToolRegistry&     tools,
                                                 ToolFormat               tool_format,
                                                 const std::string&       memory_section)
{
    SystemPromptAssembly out;

    // -- Base instructions ---------------------------------------------------
    std::string base =
        "You are Locus, a workspace-bound AI assistant. You help users understand, "
        "navigate, and modify files within a specific workspace folder.\n\n"
        "## Rules\n"
        "- Use tools to read files and search the index. Never guess file contents.\n"
        "- Paginate file reads \xe2\x80\x94 request chunks, never entire large files.\n"
        "- Summarize tool results concisely before responding.\n"
        "- When modifying files, show the user what you plan to do before executing.\n"
        "- Do not execute destructive operations (delete, overwrite) without explicit confirmation.\n"
        "- Be concise. Do not repeat information the user already has.\n\n"
        "## Editing and creating files\n"
        "- Prefer `edit_file` over `write_file` for changes to existing files. It is faster, cheaper, and cannot "
        "truncate or corrupt the file. `edit_file` takes an `edits` array \xe2\x80\x94 pass one element for a single change, "
        "or several for an atomic batch (all succeed or none are written; later edits see the results of earlier edits).\n"
        "- Use `write_file` to create new files (and for full rewrites, which require `overwrite=true`). "
        "By default `write_file` refuses to replace an existing file -- set `overwrite=true` only when you truly "
        "intend to replace the whole content. `write_file` also creates any missing parent directories on the "
        "path; there is no separate directory-creation tool.\n"
        "- `edit_file` requires the file to have been seen in this session via `read_file`, "
        "`write_file`, or a prior `edit_file` -- this prevents hallucinated edits to "
        "files whose content you haven't confirmed. A file you just wrote or edited "
        "counts as seen; you do not need to re-read it.\n"
        "- Each `old_string` must match byte-for-byte (including indentation and trailing whitespace) and must be "
        "unique in the file; widen the match with surrounding context if it is not, or set `replace_all=true`.\n\n"
        "## Running shell commands\n"
        "- Use `run_command` for synchronous commands you expect to terminate: builds, tests, scripts, queries. "
        "Default timeout is 30 minutes; the call blocks the agent until the command exits.\n"
        "- Use `run_command_bg` for dev servers, file watchers, and anything that streams output without "
        "terminating on its own. "
        "It returns a `process_id` that survives across turns. Read its output with `read_process_output` "
        "(omit `since_offset` to get only what's new since your last read), terminate with `stop_process`, "
        "and enumerate live processes with `list_processes`.\n\n";

    // -- Workspace metadata --------------------------------------------------
    std::ostringstream metadata_ss;
    metadata_ss << "## Workspace\n"
                << "- Root: " << meta.root.string() << "\n"
                << "- Indexed files: " << meta.file_count << "\n"
                << "- Code symbols: " << meta.symbol_count << "\n"
                << "- Headings: " << meta.heading_count << "\n\n";
    std::string metadata = metadata_ss.str();

    // -- LOCUS.md ------------------------------------------------------------
    std::string locus_md_section;
    if (!locus_md.empty()) {
        SystemPromptBuilder::check_locus_md_budget(locus_md);
        locus_md_section = "## LOCUS.md (workspace guide provided by the user)\n\n";
        locus_md_section += locus_md;
        locus_md_section += "\n\n";
    }

    // -- Memory Bank slot (S4.R) --------------------------------------------
    std::string memory_block;
    if (!memory_section.empty()) {
        memory_block = memory_section;
        if (memory_block.empty() || memory_block.back() != '\n')
            memory_block += '\n';
        memory_block += '\n';
    }

    // -- Available tools -----------------------------------------------------
    std::ostringstream tools_ss;
    tools_ss << "## Available Tools\n\n";
    for (auto* tool : tools.all()) {
        if (!tool->visible_in_mode(ToolMode::agent)) continue;
        tools_ss << "- **" << tool->name() << "**: " << tool->description() << "\n";
        for (auto& p : tool->params()) {
            tools_ss << "  - `" << p.name << "` (" << p.type;
            if (!p.required) tools_ss << ", optional";
            tools_ss << "): " << p.description << "\n";
        }
    }
    std::string tools_section = tools_ss.str();

    // -- Tool-call wire format addendum (S4.N) ------------------------------
    std::string addendum = tool_format_instructions(tool_format);

    // Assemble.
    out.full_text_  = base;
    out.full_text_ += metadata;
    out.full_text_ += locus_md_section;
    out.full_text_ += memory_block;
    out.full_text_ += tools_section;
    out.full_text_ += addendum;

    out.base_tokens_             = TokenCounter::estimate(base);
    out.metadata_tokens_         = TokenCounter::estimate(metadata);
    out.locus_md_tokens_         = TokenCounter::estimate(locus_md_section);
    out.memory_tokens_           = TokenCounter::estimate(memory_block);
    out.manifest_tokens_         = TokenCounter::estimate(tools_section);
    out.format_addendum_tokens_  = TokenCounter::estimate(addendum);

    out.hash_ = std::hash<std::string>{}(out.full_text_);

    return out;
}

} // namespace locus

#include "system_prompt_assembly.h"

#include "core/workspace_services.h"
#include "llm/token_counter.h"

#include <spdlog/spdlog.h>

#include <functional>
#include <sstream>

namespace locus {

const char* to_string(SystemPromptProfile p)
{
    switch (p) {
        case SystemPromptProfile::Full:    return "full";
        case SystemPromptProfile::Compact: return "compact";
        case SystemPromptProfile::Minimal: return "minimal";
    }
    return "full";
}

SystemPromptProfile profile_from_string(const std::string& s)
{
    if (s == "compact") return SystemPromptProfile::Compact;
    if (s == "minimal") return SystemPromptProfile::Minimal;
    if (s != "full" && !s.empty()) {
        spdlog::warn("Unknown system_prompt.profile '{}'; defaulting to 'full'", s);
    }
    return SystemPromptProfile::Full;
}

namespace {

// S6.12 -- the three prose bodies. Each is the entire "## Rules / Editing /
// Shell / MSVC pitfalls" section (sans the lead-in sentence which lives in
// `lead_in()` so all three profiles share the same model framing).

const char* lead_in()
{
    return "You are Locus, a workspace-bound AI assistant. You help users "
           "understand, navigate, and modify files within a specific workspace "
           "folder.\n\n";
}

const char* prose_full()
{
    return
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
        "and enumerate live processes with `list_processes`.\n\n"
        // S5.Z follow-up: small set of platform-specific pitfalls that
        // burned rounds in agentic Tetris testing. Cheap (~60 tokens) and
        // saves entire LLM round-trips when the model would otherwise
        // produce code that fails to compile on first build. Add new
        // entries here when a repeated failure pattern shows up.
        "## Windows C++ / MSVC pitfalls\n"
        "- Before `#include <windows.h>`, write `#define NOMINMAX` (otherwise `windows.h` defines "
        "`min` and `max` as preprocessor macros that break `std::max(...)` / `std::min(...)`).\n"
        "- Do not assign braced initializer lists to `std::array` members after construction. "
        "Either brace-initialize them as `static const` at namespace scope, or fill them with "
        "`memcpy` / a loop in a small `init()` function.\n\n";
}

const char* prose_compact()
{
    return
        "## Rules\n"
        "- Use tools to read files and search the index. Never guess file contents.\n"
        "- Paginate file reads. Never request a whole large file.\n"
        "- Summarize tool results before responding.\n"
        "- If a search returns weak results, retry with a different query on "
        "the SAME topic (try function names or identifiers related to the "
        "user's question -- do not switch topics or repeat the same words).\n"
        "- Be concise.\n\n"
        "## Editing files\n"
        "- Prefer `edit_file` over `write_file` for existing files. `edit_file` requires the path to have been "
        "seen via `read_file` / `write_file` / a prior `edit_file` in this session.\n"
        "- Use `write_file` for new files. Set `overwrite=true` to replace; the default refuses.\n\n"
        "## Running shell commands\n"
        "- `run_command` blocks until the command exits. `run_command_bg` returns a `process_id` you drain with "
        "`read_process_output` and terminate with `stop_process`.\n\n"
        "## Windows C++ / MSVC pitfalls\n"
        "- `#define NOMINMAX` before `#include <windows.h>`.\n"
        "- Do not assign braced initializer lists to `std::array` members after construction.\n\n";
}

const char* prose_minimal()
{
    return
        "## Rules\n"
        "- Use tools, do not guess.\n"
        "- Paginate reads.\n"
        "- `edit_file` requires a prior `read_file` on the same path this session.\n"
        "- Be concise.\n\n";
}

const char* prose_for(SystemPromptProfile p)
{
    switch (p) {
        case SystemPromptProfile::Full:    return prose_full();
        case SystemPromptProfile::Compact: return prose_compact();
        case SystemPromptProfile::Minimal: return prose_minimal();
    }
    return prose_full();
}

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
                                                 const std::string&       memory_section,
                                                 bool                     lazy_manifest,
                                                 IWorkspaceServices*      ws_for_filter,
                                                 SystemPromptProfile      profile)
{
    SystemPromptAssembly out;
    out.profile_ = profile;

    // -- Base instructions ---------------------------------------------------
    // S6.12 -- prose body picked by profile. Lead-in is shared across all
    // three so the model framing stays stable; Rules / Editing / Shell /
    // MSVC pitfalls vary.
    std::string base = lead_in();
    base += prose_for(profile);

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
    // S6.11 -- when `lazy_manifest` is on, the section degrades to per-tool
    // one-liners (no param breakdown). A header hint tells the model how to
    // fetch full schemas via the describe_tool meta-tool.
    // S6.10 Task I -- under lazy_manifest, render each tool's curated
    // `short_description()` instead of the full `description()` so the prose
    // section shrinks roughly proportionally to the API tools array.
    // S6.10 Task J -- when lazy_manifest is on AND the wire format is
    // openai_json / auto, the prose section is pure duplication of the API
    // tools[] array the LLM already consumes. Render only a one-line pointer
    // so the model still knows describe_tool exists, but skip the per-tool
    // bullets entirely. For qwen_xml / claude_xml / none, the prose section
    // IS the tool listing (those formats don't consume the API array) -- keep
    // the full per-tool bullets there.
    const bool skip_prose_tool_list =
        lazy_manifest &&
        (tool_format == ToolFormat::OpenAi || tool_format == ToolFormat::Auto);

    std::ostringstream tools_ss;
    if (skip_prose_tool_list) {
        tools_ss << "## Tools\n"
                    "See the API `tools[]` array for the available tools and their summaries. "
                    "Call `describe_tool('<name>')` for the full JSON schema of any tool "
                    "before you invoke it.\n\n";
    } else {
        if (lazy_manifest)
            tools_ss << "## Available Tools (summaries -- call describe_tool('<name>') for the full schema)\n\n";
        else
            tools_ss << "## Available Tools\n\n";

        for (auto* tool : tools.all()) {
            if (!tool->visible_in_mode(ToolMode::agent)) continue;
            // Mirror the API tools-array filter so describe_tool is hidden from
            // the system prompt when lazy_manifest=false (its `available(ws)`
            // returns false in that case). Skip the workspace filter only when
            // no workspace is available (test builds).
            if (ws_for_filter && !tool->available(*ws_for_filter)) continue;

            const std::string& desc = lazy_manifest
                                          ? tool->short_description()
                                          : tool->description();
            tools_ss << "- **" << tool->name() << "**: " << desc << "\n";
            if (!lazy_manifest) {
                for (auto& p : tool->params()) {
                    tools_ss << "  - `" << p.name << "` (" << p.type;
                    if (!p.required) tools_ss << ", optional";
                    tools_ss << "): " << p.description << "\n";
                }
            }
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

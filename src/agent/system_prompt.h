#pragma once

#include "llm/llm_client.h"   // ToolFormat
#include "../tools/tool.h"

#include <filesystem>
#include <string>

namespace locus {

// Assembles the system prompt from its constituent parts:
//   1. Base instructions (hardcoded agent behavior)
//   2. LOCUS.md content (user-provided workspace guide)
//   3. Workspace metadata (root path, file count, index stats)
//   4. Tool schema (injected as text, not as the API tools array)
//
// The tool schema is also available separately via IToolRegistry::build_schema_json()
// for the OpenAI tools parameter. This text version goes into the system prompt
// so the LLM understands tool semantics in its context window.
//
// ## Invariant (S4.F): byte-stable system prompt across a session
//
// `SystemPromptBuilder::build()` output MUST be byte-stable across turns
// within a single session. LM Studio / llama.cpp prefix caching only fires
// when the leading bytes (system message in particular) are identical to the
// previous request -- any per-turn change forces a full re-prefill of the
// system prompt + tool manifest (2K+ tokens; 10-30 s on a 26B model).
//
// Anything that varies per turn -- timestamps, live file counts, last-indexed
// times, the attached-file outline, file-change deltas, retrieved memory --
// belongs on a user message (in the volatile tail of the conversation),
// **not** here. Workspace metadata is captured once at construction and
// deliberately not refreshed mid-session for the same reason; if a future
// stage needs fresh counts, expose them via a tool (`get_workspace_stats`)
// rather than reaching back into this builder.
//
// LOCUS.md and the tool manifest are allowed to change *between sessions*
// (workspace reload, tool registry edits); the cache miss on the first
// turn after such an event is appropriate.

struct WorkspaceMetadata {
    std::filesystem::path root;
    int file_count    = 0;
    int symbol_count  = 0;
    int heading_count = 0;
};

class SystemPromptBuilder {
public:
    // Build the full system prompt string. `tool_format` selects an
    // optional appendix that tells the model exactly how to emit tool
    // calls when the wire format isn't pure OpenAI JSON -- a hedge for
    // models whose chat template is broken or absent in LM Studio.
    //
    // `memory_section` (S4.R) is the pre-rendered "## Memory Bank" block
    // produced by `MemoryStore::format_for_system_prompt`. Empty when memory
    // is disabled or there are no entries to surface; the builder then
    // suppresses the section entirely so the prompt stays clean on first run.
    static std::string build(const std::string&       locus_md,
                             const WorkspaceMetadata& meta,
                             const IToolRegistry&     tools,
                             ToolFormat               tool_format    = ToolFormat::Auto,
                             const std::string&       memory_section = "");

    // Estimate tokens for a LOCUS.md string. Logs a warning if > 500 tokens.
    static int check_locus_md_budget(const std::string& locus_md);
};

} // namespace locus

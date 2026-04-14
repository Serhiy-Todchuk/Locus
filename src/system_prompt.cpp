#include "system_prompt.h"
#include "llm_client.h"

#include <spdlog/spdlog.h>

#include <sstream>

namespace locus {

std::string SystemPromptBuilder::build(const std::string& locus_md,
                                       const WorkspaceMetadata& meta,
                                       const IToolRegistry& tools)
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
          "- Be concise. Do not repeat information the user already has.\n\n";

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

    return ss.str();
}

int SystemPromptBuilder::check_locus_md_budget(const std::string& locus_md)
{
    int tokens = ILLMClient::estimate_tokens(locus_md);
    if (tokens > 500) {
        spdlog::warn("LOCUS.md is ~{} tokens (budget: 500). Consider trimming it "
                     "to leave more context for conversation.", tokens);
    } else {
        spdlog::trace("LOCUS.md token estimate: {}", tokens);
    }
    return tokens;
}

} // namespace locus

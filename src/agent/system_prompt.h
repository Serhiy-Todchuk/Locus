#pragma once

#include "tool.h"

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

struct WorkspaceMetadata {
    std::filesystem::path root;
    int file_count    = 0;
    int symbol_count  = 0;
    int heading_count = 0;
};

class SystemPromptBuilder {
public:
    // Build the full system prompt string.
    static std::string build(const std::string& locus_md,
                             const WorkspaceMetadata& meta,
                             const IToolRegistry& tools);

    // Estimate tokens for a LOCUS.md string. Logs a warning if > 500 tokens.
    static int check_locus_md_budget(const std::string& locus_md);
};

} // namespace locus

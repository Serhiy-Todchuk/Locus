#include "system_prompt.h"
#include "system_prompt_assembly.h"
#include "llm/token_counter.h"

#include <spdlog/spdlog.h>

namespace locus {

// S5.J -- SystemPromptBuilder is now a thin forwarder over SystemPromptAssembly.
// The byte-stable invariant (S4.F) used to live as discipline on the AgentCore
// side ("don't recompute base_system_prompt_"); after S5.J it lives in the
// type system -- SystemPromptAssembly is immutable post-construction and has
// no setter. SystemPromptBuilder::build() stays for back-compat with callers
// (and a handful of existing tests) that just want the raw string.

std::string SystemPromptBuilder::build(const std::string&       locus_md,
                                       const WorkspaceMetadata& meta,
                                       const IToolRegistry&     tools,
                                       ToolFormat               tool_format,
                                       const std::string&       memory_section)
{
    return SystemPromptAssembly::build(locus_md, meta, tools,
                                       tool_format, memory_section).full_text();
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

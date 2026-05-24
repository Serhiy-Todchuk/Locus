#pragma once

#include "system_prompt.h"          // WorkspaceMetadata
#include "llm/llm_client.h"         // ToolFormat
#include "../tools/tool.h"

#include <cstddef>
#include <string>

namespace locus {

class IWorkspaceServices;


// Immutable, byte-stable system-prompt assembly (S5.J).
//
// The result of SystemPromptBuilder::build() used to be a raw std::string
// owned by AgentCore and re-seeded across attach/detach/reset paths. The
// "byte-stable across a session" rule that LM Studio / llama.cpp prefix
// caching relies on (S4.F) lived as discipline -- "don't rewrite
// base_system_prompt_". SystemPromptAssembly turns that into a type-level
// guarantee: the only way to obtain one is `build()`, every reader is
// const, and there is no setter. A new prompt = a new assembly object;
// the bytes inside cannot change once built.
//
// Per-section token counts are exposed so the future S5.G chat panel can
// render a stacked bar without re-deriving them. `total_tokens()` is the
// sum of the per-section values (not a re-estimate of `full_text()`),
// so the breakdown is exact by construction.
class SystemPromptAssembly {
public:
    // The only way to construct a populated SystemPromptAssembly. Mirrors
    // SystemPromptBuilder::build() so the byte output is identical for the
    // same inputs.
    //
    // S6.11 -- `lazy_manifest=true` collapses the "## Available Tools" section
    // to one-line summaries (no per-param lines), and adds a hint pointing the
    // model at `describe_tool('<name>')` for the full schema. Workspace
    // metadata / LOCUS.md / memory bank / format addendum are not affected.
    // The `IWorkspaceServices*` argument is passed so per-tool filtering
    // (`available(ws)`, `visible_in_mode`) matches the API tools array, which
    // gates the describe_tool meta-tool when lazy_manifest is off.
    static SystemPromptAssembly build(const std::string&       locus_md,
                                      const WorkspaceMetadata& meta,
                                      const IToolRegistry&     tools,
                                      ToolFormat               tool_format    = ToolFormat::Auto,
                                      const std::string&       memory_section = "",
                                      bool                     lazy_manifest  = false,
                                      IWorkspaceServices*      ws_for_filter  = nullptr);

    // Empty default assembly -- useful when an LLMContext needs to be
    // default-constructed in a test and the prompt is set later.
    SystemPromptAssembly() = default;

    SystemPromptAssembly(const SystemPromptAssembly&)            = default;
    SystemPromptAssembly(SystemPromptAssembly&&) noexcept        = default;
    SystemPromptAssembly& operator=(const SystemPromptAssembly&) = default;
    SystemPromptAssembly& operator=(SystemPromptAssembly&&) noexcept = default;

    const std::string& full_text() const { return full_text_; }
    bool empty()  const { return full_text_.empty(); }
    std::size_t hash() const { return hash_; }

    int base_tokens()              const { return base_tokens_; }
    int metadata_tokens()          const { return metadata_tokens_; }
    int locus_md_tokens()          const { return locus_md_tokens_; }
    int memory_tokens()            const { return memory_tokens_; }
    int manifest_tokens()          const { return manifest_tokens_; }
    int format_addendum_tokens()   const { return format_addendum_tokens_; }

    // Sum of every section's token estimate. Exact by construction, so
    // per-section breakdowns always add up to the total visible in chrome.
    int total_tokens() const {
        return base_tokens_ + metadata_tokens_ + locus_md_tokens_
             + memory_tokens_ + manifest_tokens_ + format_addendum_tokens_;
    }

private:
    std::string full_text_;
    std::size_t hash_                = 0;
    int         base_tokens_         = 0;
    int         metadata_tokens_     = 0;
    int         locus_md_tokens_     = 0;
    int         memory_tokens_       = 0;
    int         manifest_tokens_     = 0;
    int         format_addendum_tokens_ = 0;
};

} // namespace locus

// S6.10 Tasks I + J -- live re-measurement helper.
//
// Hidden Catch2 test (tag prefix `.` keeps it out of the default run). Prints
// a four-row budget table comparing:
//
//   1. full mode + Auto                      -- the "before S6.11" baseline
//   2. lazy + Qwen                           -- S6.11 + Task I (Task J inactive)
//   3. lazy + Claude                         -- S6.11 + Task I (Task J inactive)
//   4. lazy + OpenAi (Task I + J both fire)  -- the headline saving
//   5. lazy + Auto   (Task I + J both fire)  -- same as openai_json by spec
//
// For each cell we report:
//   - system-prompt total tokens (TokenCounter::estimate on full_text)
//   - prose tools-section tokens (SystemPromptAssembly::manifest_tokens())
//   - API tools[] array JSON tokens (TokenCounter::estimate on JSON dump)
//   - API tools[] array JSON bytes
//
// NOTE: this fixture uses FakeWorkspaceServices, which means `describe_tool`
// is filtered out of the manifest (its `available()` returns false without a
// real Workspace handle). The 2026-05-25 baseline at TestLocalVibe was
// measured against a real workspace and so included describe_tool. Expect
// the absolute lazy numbers here to run ~120-180 t lower than the real
// workspace number for the same setup; the relative shape (Task I shrink,
// Task J cliff) is what matters.
//
// Run with:
//   build/release/tests/Release/locus_tests.exe "[.s6_10_budget]" --reporter compact

#include "agent/system_prompt_assembly.h"
#include "llm/token_counter.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <iomanip>
#include <iostream>
#include <string>

namespace {

using locus::IWorkspaceServices;
using locus::SystemPromptAssembly;
using locus::ToolFormat;
using locus::ToolMode;
using locus::ToolRegistry;
using locus::TokenCounter;
using locus::WorkspaceMetadata;

struct Combo {
    bool         lazy;
    ToolFormat   fmt;
    const char*  label;
};

const char* fmt_str(ToolFormat f)
{
    switch (f) {
        case ToolFormat::OpenAi: return "openai";
        case ToolFormat::Qwen:   return "qwen";
        case ToolFormat::Claude: return "claude";
        case ToolFormat::Auto:   return "auto";
        case ToolFormat::None:   return "none";
    }
    return "?";
}

}  // namespace

TEST_CASE("S6.10 manifest budget measurement (Tasks I + J)",
          "[.s6_10_budget]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);

    locus::test::FakeWorkspaceServices ws("d:/Projects/AICodeAss");

    WorkspaceMetadata meta;
    meta.root          = "d:/Projects/AICodeAss";
    meta.file_count    = 0;
    meta.symbol_count  = 0;
    meta.heading_count = 0;

    const Combo combos[] = {
        {false, ToolFormat::Auto,   "full + auto"},
        {true,  ToolFormat::Qwen,   "lazy + qwen"},
        {true,  ToolFormat::Claude, "lazy + claude"},
        {true,  ToolFormat::OpenAi, "lazy + openai"},
        {true,  ToolFormat::Auto,   "lazy + auto"},
    };

    std::cout << "\n";
    std::cout << "S6.10 manifest budget -- FakeWorkspaceServices (describe_tool not counted)\n";
    std::cout << std::left
              << std::setw(18) << "config"
              << std::right
              << std::setw(11) << "sys_total"
              << std::setw(13) << "sys_prose"
              << std::setw(13) << "api_tokens"
              << std::setw(11) << "api_bytes"
              << "\n";
    std::cout << std::string(66, '-') << "\n";

    for (const auto& c : combos) {
        auto a = SystemPromptAssembly::build("", meta, reg, c.fmt, "",
                                             /*lazy_manifest=*/c.lazy,
                                             /*ws_for_filter=*/&ws);
        auto tools = reg.build_schema_json(ws, ToolMode::agent, c.lazy);
        std::string tools_str = tools.dump();
        int tools_tokens = TokenCounter::estimate(tools_str);

        std::cout << std::left
                  << std::setw(18) << c.label
                  << std::right
                  << std::setw(11) << a.total_tokens()
                  << std::setw(13) << a.manifest_tokens()
                  << std::setw(13) << tools_tokens
                  << std::setw(11) << tools_str.size()
                  << "\n";
    }
    std::cout << std::string(66, '-') << "\n";
    std::cout << "Format key:  sys_total  = system prompt total tokens\n";
    std::cout << "             sys_prose  = '## Available Tools' section tokens\n";
    std::cout << "             api_tokens = API tools[] JSON token estimate\n";
    std::cout << "             api_bytes  = API tools[] JSON byte count\n\n";

    // Per-format-fixed sanity asserts so the test fails loudly if the shape
    // ever breaks (e.g. an accidental change that doubles the prose section).
    auto a_full   = SystemPromptAssembly::build("", meta, reg, ToolFormat::Auto, "",
                                                /*lazy=*/false, &ws);
    auto a_lazy_q = SystemPromptAssembly::build("", meta, reg, ToolFormat::Qwen, "",
                                                /*lazy=*/true,  &ws);
    auto a_lazy_o = SystemPromptAssembly::build("", meta, reg, ToolFormat::OpenAi, "",
                                                /*lazy=*/true,  &ws);

    // Task I: lazy + qwen prose section is shorter than full prose.
    REQUIRE(a_lazy_q.manifest_tokens() < a_full.manifest_tokens());
    // Task J: lazy + openai prose section is shorter still than lazy + qwen.
    REQUIRE(a_lazy_o.manifest_tokens() < a_lazy_q.manifest_tokens());
}

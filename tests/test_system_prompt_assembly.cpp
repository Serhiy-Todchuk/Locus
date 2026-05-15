// S5.J -- SystemPromptAssembly unit tests.
//
// Covers:
//   - hash() stability across two builds with the same inputs (the byte-stable
//     invariant the S4.F KV-cache discipline used to enforce by hand)
//   - hash() changes when any input changes (sanity: no false-equal hashes
//     across "same workspace, different LOCUS.md" etc.)
//   - per-section token breakdown sums to total_tokens()
//   - SystemPromptBuilder::build() back-compat: forwarder produces the same
//     bytes the SystemPromptAssembly::build() does

#include <catch2/catch_test_macros.hpp>

#include "agent/system_prompt.h"
#include "agent/system_prompt_assembly.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"

using namespace locus;

namespace {

WorkspaceMetadata make_meta()
{
    WorkspaceMetadata m;
    m.root          = "d:/test/workspace";
    m.file_count    = 42;
    m.symbol_count  = 100;
    m.heading_count = 15;
    return m;
}

ToolRegistry& shared_registry()
{
    static ToolRegistry reg;
    static bool registered = false;
    if (!registered) { register_builtin_tools(reg); registered = true; }
    return reg;
}

} // namespace

TEST_CASE("SystemPromptAssembly: build() is deterministic", "[s5.j][system-prompt-assembly]")
{
    auto& reg = shared_registry();
    auto meta = make_meta();
    std::string locus_md = "# Project\nA test workspace.";

    auto a = SystemPromptAssembly::build(locus_md, meta, reg);
    auto b = SystemPromptAssembly::build(locus_md, meta, reg);

    REQUIRE(a.full_text() == b.full_text());
    REQUIRE(a.hash()      == b.hash());
    REQUIRE(a.total_tokens() == b.total_tokens());
}

TEST_CASE("SystemPromptAssembly: hash changes when LOCUS.md changes",
          "[s5.j][system-prompt-assembly]")
{
    auto& reg = shared_registry();
    auto meta = make_meta();

    auto a = SystemPromptAssembly::build("# A", meta, reg);
    auto b = SystemPromptAssembly::build("# B", meta, reg);

    REQUIRE(a.hash() != b.hash());
    REQUIRE(a.full_text() != b.full_text());
}

TEST_CASE("SystemPromptAssembly: hash changes when metadata changes",
          "[s5.j][system-prompt-assembly]")
{
    auto& reg = shared_registry();
    WorkspaceMetadata m1 = make_meta();
    WorkspaceMetadata m2 = make_meta();
    m2.file_count = m1.file_count + 1;

    auto a = SystemPromptAssembly::build("", m1, reg);
    auto b = SystemPromptAssembly::build("", m2, reg);

    REQUIRE(a.hash() != b.hash());
}

TEST_CASE("SystemPromptAssembly: per-section token breakdown sums to total",
          "[s5.j][system-prompt-assembly]")
{
    auto& reg = shared_registry();
    auto meta = make_meta();
    std::string locus_md = "# Project\nA test workspace.";

    auto a = SystemPromptAssembly::build(locus_md, meta, reg);

    int sum = a.base_tokens()
            + a.metadata_tokens()
            + a.locus_md_tokens()
            + a.memory_tokens()
            + a.manifest_tokens()
            + a.format_addendum_tokens();
    REQUIRE(a.total_tokens() == sum);

    // Sanity: at least the base + metadata + tool manifest are non-empty for
    // a real workspace with built-in tools.
    REQUIRE(a.base_tokens()     > 0);
    REQUIRE(a.metadata_tokens() > 0);
    REQUIRE(a.manifest_tokens() > 0);
    // LOCUS.md provided -> non-zero. Memory absent -> zero. Addendum Auto -> zero.
    REQUIRE(a.locus_md_tokens()  > 0);
    REQUIRE(a.memory_tokens()         == 0);
    REQUIRE(a.format_addendum_tokens() == 0);
}

TEST_CASE("SystemPromptAssembly: memory section bumps memory_tokens",
          "[s5.j][system-prompt-assembly]")
{
    auto& reg = shared_registry();
    auto meta = make_meta();
    std::string memory_section =
        "## Memory Bank\n- [pinned] You always use four-space indentation.\n";

    auto no_mem   = SystemPromptAssembly::build("", meta, reg);
    auto with_mem = SystemPromptAssembly::build("", meta, reg,
                                                ToolFormat::Auto, memory_section);

    REQUIRE(with_mem.memory_tokens() > 0);
    REQUIRE(no_mem.memory_tokens()   == 0);
    REQUIRE(with_mem.hash() != no_mem.hash());
}

TEST_CASE("SystemPromptAssembly: format addendum included for non-Auto formats",
          "[s5.j][system-prompt-assembly]")
{
    auto& reg = shared_registry();
    auto meta = make_meta();

    auto a_auto  = SystemPromptAssembly::build("", meta, reg, ToolFormat::Auto);
    auto a_qwen  = SystemPromptAssembly::build("", meta, reg, ToolFormat::Qwen);
    auto a_none  = SystemPromptAssembly::build("", meta, reg, ToolFormat::None);

    REQUIRE(a_auto.format_addendum_tokens() == 0);
    REQUIRE(a_qwen.format_addendum_tokens() > 0);
    REQUIRE(a_none.format_addendum_tokens() > 0);
    REQUIRE(a_auto.hash() != a_qwen.hash());
    REQUIRE(a_qwen.hash() != a_none.hash());
}

TEST_CASE("SystemPromptBuilder::build forwards to SystemPromptAssembly",
          "[s5.j][system-prompt-assembly]")
{
    auto& reg = shared_registry();
    auto meta = make_meta();
    std::string locus_md = "# Project\nA test workspace.";

    auto assembly  = SystemPromptAssembly::build(locus_md, meta, reg);
    std::string s  = SystemPromptBuilder::build(locus_md, meta, reg);

    REQUIRE(assembly.full_text() == s);
}

TEST_CASE("SystemPromptAssembly: default-constructed is empty",
          "[s5.j][system-prompt-assembly]")
{
    SystemPromptAssembly a;
    REQUIRE(a.empty());
    REQUIRE(a.full_text().empty());
    REQUIRE(a.total_tokens() == 0);
    REQUIRE(a.hash() == 0);
}

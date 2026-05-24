// S6.12 -- system-prompt profile tests.
//
// Coverage:
// - to_string / profile_from_string round-trips for Full / Compact / Minimal.
// - Unknown string falls back to Full (with warn-log).
// - Each profile produces a deterministic rendered body (same inputs ->
//   same hash, two builds in a row).
// - Compact body is strictly shorter than Full; Minimal is strictly shorter
//   than Compact.
// - Switching profile invalidates the SystemPromptAssembly::hash() canary
//   (S4.F prefix-cache invalidation key).
// - Full profile remains byte-identical to what SystemPromptBuilder::build()
//   produces (regression guard against accidental whitespace drift).
// - Workspace metadata / LOCUS.md / memory bank / tools section render the
//   same in every profile (they are NOT what the profile cuts).

#include "agent/system_prompt.h"
#include "agent/system_prompt_assembly.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <catch2/catch_test_macros.hpp>

namespace {

using locus::SystemPromptAssembly;
using locus::SystemPromptBuilder;
using locus::SystemPromptProfile;
using locus::ToolFormat;
using locus::ToolRegistry;
using locus::WorkspaceMetadata;
using locus::profile_from_string;
using locus::to_string;

WorkspaceMetadata sample_meta()
{
    WorkspaceMetadata m;
    m.root          = "/tmp/wstest";
    m.file_count    = 42;
    m.symbol_count  = 100;
    m.heading_count = 5;
    return m;
}

}  // namespace

TEST_CASE("system_prompt_profile: to_string / from_string round trip",
          "[s6.12][system_prompt]")
{
    REQUIRE(std::string(to_string(SystemPromptProfile::Full))    == "full");
    REQUIRE(std::string(to_string(SystemPromptProfile::Compact)) == "compact");
    REQUIRE(std::string(to_string(SystemPromptProfile::Minimal)) == "minimal");

    REQUIRE(static_cast<int>(profile_from_string("full"))    == static_cast<int>(SystemPromptProfile::Full));
    REQUIRE(static_cast<int>(profile_from_string("compact")) == static_cast<int>(SystemPromptProfile::Compact));
    REQUIRE(static_cast<int>(profile_from_string("minimal")) == static_cast<int>(SystemPromptProfile::Minimal));
    // Empty -> default Full (no warn).
    REQUIRE(static_cast<int>(profile_from_string(""))        == static_cast<int>(SystemPromptProfile::Full));
    // Unknown string -> Full with warn.
    REQUIRE(static_cast<int>(profile_from_string("bogus"))   == static_cast<int>(SystemPromptProfile::Full));
}

TEST_CASE("system_prompt_profile: deterministic across two builds",
          "[s6.12][system_prompt][s4.f]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);

    auto a = SystemPromptAssembly::build("", sample_meta(), reg,
                                          ToolFormat::Auto, "",
                                          /*lazy_manifest=*/false,
                                          /*ws=*/nullptr,
                                          SystemPromptProfile::Compact);
    auto b = SystemPromptAssembly::build("", sample_meta(), reg,
                                          ToolFormat::Auto, "",
                                          /*lazy_manifest=*/false,
                                          /*ws=*/nullptr,
                                          SystemPromptProfile::Compact);
    REQUIRE(a.full_text() == b.full_text());
    REQUIRE(a.hash() == b.hash());
    REQUIRE(static_cast<int>(a.profile()) == static_cast<int>(SystemPromptProfile::Compact));
}

TEST_CASE("system_prompt_profile: Compact < Full and Minimal < Compact",
          "[s6.12][system_prompt]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);

    auto full    = SystemPromptAssembly::build("", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Full);
    auto compact = SystemPromptAssembly::build("", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Compact);
    auto minimal = SystemPromptAssembly::build("", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Minimal);

    REQUIRE(compact.full_text().size() < full.full_text().size());
    REQUIRE(minimal.full_text().size() < compact.full_text().size());

    // S4.F cache-invalidation canary: three distinct hashes.
    REQUIRE(full.hash() != compact.hash());
    REQUIRE(compact.hash() != minimal.hash());
    REQUIRE(full.hash() != minimal.hash());

    // base_tokens decreases monotonically (the prose body is what shrinks).
    REQUIRE(compact.base_tokens() < full.base_tokens());
    REQUIRE(minimal.base_tokens() < compact.base_tokens());
}

TEST_CASE("system_prompt_profile: Full profile remains byte-identical to forwarder",
          "[s6.12][system_prompt][regression]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);

    // SystemPromptBuilder::build() (S5.J back-compat forwarder) uses the
    // default Full profile via SystemPromptAssembly::build() default-arg.
    std::string forwarded = SystemPromptBuilder::build("", sample_meta(), reg);
    auto direct = SystemPromptAssembly::build("", sample_meta(), reg,
                                              ToolFormat::Auto, "", false, nullptr,
                                              SystemPromptProfile::Full);
    REQUIRE(forwarded == direct.full_text());
}

TEST_CASE("system_prompt_profile: workspace metadata + tools section identical across profiles",
          "[s6.12][system_prompt]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);

    auto full    = SystemPromptAssembly::build("# LOCUS", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Full);
    auto compact = SystemPromptAssembly::build("# LOCUS", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Compact);
    auto minimal = SystemPromptAssembly::build("# LOCUS", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Minimal);

    REQUIRE(full.metadata_tokens()    == compact.metadata_tokens());
    REQUIRE(compact.metadata_tokens() == minimal.metadata_tokens());

    REQUIRE(full.locus_md_tokens()    == compact.locus_md_tokens());
    REQUIRE(compact.locus_md_tokens() == minimal.locus_md_tokens());

    REQUIRE(full.manifest_tokens()    == compact.manifest_tokens());
    REQUIRE(compact.manifest_tokens() == minimal.manifest_tokens());

    REQUIRE(full.format_addendum_tokens()    == compact.format_addendum_tokens());
    REQUIRE(compact.format_addendum_tokens() == minimal.format_addendum_tokens());
}

TEST_CASE("system_prompt_profile: token bands roughly match the design targets",
          "[s6.12][system_prompt]")
{
    ToolRegistry reg;
    locus::register_builtin_tools(reg);

    auto full    = SystemPromptAssembly::build("", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Full);
    auto compact = SystemPromptAssembly::build("", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Compact);
    auto minimal = SystemPromptAssembly::build("", sample_meta(), reg,
                                                ToolFormat::Auto, "", false, nullptr,
                                                SystemPromptProfile::Minimal);

    // Targets per ADR-0007: Full ~700-1000, Compact ~300, Minimal ~80.
    // Wide bands -- this is a sanity check, not an exact ruler.
    REQUIRE(full.base_tokens()    > 500);
    REQUIRE(compact.base_tokens() > 100);
    REQUIRE(compact.base_tokens() < 500);
    REQUIRE(minimal.base_tokens() > 20);
    REQUIRE(minimal.base_tokens() < 200);
}

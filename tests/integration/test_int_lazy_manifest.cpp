// S6.11 -- lazy tool manifest integration tests.
//
// Live-LLM assertions for the lazy_manifest path:
//   1. With lazy_tool_manifest=true, describe_tool is registered AND
//      reachable -- the agent can be instructed to call it and it returns a
//      structurally valid OpenAI-format schema entry for a named tool.
//   2. Unknown tool name -> describe_tool reports a closest-match suggestion.
//   3. Toggling lazy_tool_manifest mid-session doesn't crash the harness.
//
// We do NOT assert "the model spontaneously calls describe_tool when it
// doesn't know a tool" -- that is a model-quality property that varies per
// model. The deterministic behaviour we care about is that the tool is
// reachable and produces the expected output when explicitly invoked.

#include "harness_fixture.h"
#include "core/workspace.h"
#include "core/workspace_config.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace locus::integration;

namespace {

// Set lazy_tool_manifest for the harness's workspace and remember the old
// value so the test fixture can restore it. The harness is shared across
// every test in this binary; mutating config without restoring would leak
// across cases in unpredictable order.
class LazyManifestGuard {
public:
    explicit LazyManifestGuard(bool enable)
        : prev_(harness().workspace().config().agent.lazy_tool_manifest)
    {
        harness().workspace().config().agent.lazy_tool_manifest = enable;
    }
    ~LazyManifestGuard()
    {
        harness().workspace().config().agent.lazy_tool_manifest = prev_;
    }
private:
    bool prev_;
};

} // namespace

TEST_CASE("describe_tool returns a valid schema for a known tool when lazy_manifest is on",
          "[integration][llm][s6.11][lazy_manifest]")
{
    auto& h = harness();
    LazyManifestGuard lazy(true);

    PromptResult r = h.prompt(
        "Call the describe_tool tool with name=\"read_file\". Then quote the "
        "first 60 characters of its returned content verbatim.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("describe_tool"));

    const auto* res = r.find_result_for("describe_tool");
    REQUIRE(res != nullptr);
    REQUIRE_FALSE(res->display.empty());

    // The content is a JSON dump; key shape markers we expect regardless of
    // how the model paraphrases the result into its reply.
    REQUIRE(res->display.find("read_file") != std::string::npos);
    REQUIRE(res->display.find("\"type\":") != std::string::npos);
    REQUIRE(res->display.find("\"function\":") != std::string::npos);
}

TEST_CASE("describe_tool reports closest-match suggestion for an unknown tool name",
          "[integration][llm][s6.11][lazy_manifest]")
{
    auto& h = harness();
    LazyManifestGuard lazy(true);

    // "read_filex" is one character past "read_file" -- the levenshtein helper
    // should land on read_file as the suggestion. Suffix-added rather than
    // separator-dropped because small models routinely "helpfully" autocorrect
    // a name with a missing underscore ("readfile" -> "read_file") before
    // shipping the tool call, which trips the closest-match path before our
    // code can see the bad input. Adding a trailing character is far less
    // likely to be auto-fixed (it looks deliberate, not like a typo) so the
    // test exercises the actual closest-match code on small models too.
    // Verified on gemma-4-e4b 2026-05-25.
    PromptResult r = h.prompt(
        "Call the describe_tool tool with the argument `name` set to the "
        "EXACT string \"read_filex\" -- do not modify, autocorrect, or strip "
        "the trailing 'x'. The tool will reject the name with an error; quote "
        "the error verbatim.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("describe_tool"));

    const auto* res = r.find_result_for("describe_tool");
    REQUIRE(res != nullptr);
    // success=false renders as the error body without the schema markers.
    REQUIRE(res->display.find("not registered") != std::string::npos);
    // The suggestion path landed on read_file. (Whichever exact variant of
    // the bad input the model shipped, the suggestion is what we care about.)
    REQUIRE(res->display.find("read_file") != std::string::npos);
    REQUIRE(res->display.find("Did you mean") != std::string::npos);
}

// S6.17 Task H -- prompt_cost preset resolution.
//
// Verifies the load-path resolution + the observable per-turn effect:
//   - prompt_cost_apply() correctly resolves `balanced` onto the
//     underlying lazy_tool_manifest + system_prompt_profile fields.
//   - The next turn's `Tool manifest: N tools, ~Mt` log line reflects the
//     lazy collapse (M well under the full-schema ~3700t mark for the
//     default 15-tool roster).
//
// Architectural note: `system_prompt_profile` is locked in at AgentCore
// construction time (the S4.F byte-stable system prompt invariant) and
// CANNOT change mid-session. So this test cannot assert `profile=compact`
// in the per-turn system-prompt log line -- the harness's AgentCore was
// built with the default profile=full and would log that regardless of
// runtime config edits. The unit tests in test_lazy_manifest.cpp
// `[s6.17][prompt_cost]` cover the resolution logic exhaustively;
// observability of the resolved fields on workspace OPEN is covered by
// the agentic acceptance run (findings.md F-NEW-3).
TEST_CASE("prompt_cost=balanced resolves flags and shrinks the per-turn manifest",
          "[integration][llm][s6.17][prompt_cost]")
{
    namespace fs = std::filesystem;
    auto& h = harness();

    // Snapshot the prior config so we can restore on scope exit -- the harness
    // is shared across tests, so leaking knobs would scramble downstream cases.
    auto& cfg = h.workspace().config();
    const bool prev_lazy    = cfg.agent.lazy_tool_manifest;
    const auto prev_profile = cfg.agent.system_prompt_profile;
    const auto prev_preset  = cfg.agent.prompt_cost;
    struct Restore {
        locus::WorkspaceConfig& cfg;
        bool lazy;
        std::string profile;
        std::string preset;
        ~Restore() {
            cfg.agent.lazy_tool_manifest    = lazy;
            cfg.agent.system_prompt_profile = profile;
            cfg.agent.prompt_cost           = preset;
        }
    } restore{cfg, prev_lazy, prev_profile, prev_preset};

    cfg.agent.prompt_cost = "balanced";
    locus::prompt_cost_apply(cfg);

    // The apply resolves onto the underlying flags.
    REQUIRE(cfg.agent.lazy_tool_manifest);
    REQUIRE(cfg.agent.system_prompt_profile == "compact");

    // Mark a byte position in the integration log BEFORE the turn fires,
    // so we only scan lines this test produced (the log accumulates across
    // every preceding case in this binary).
    const fs::path log_path = h.workspace_root() / ".locus" / "integration_test.log";
    std::size_t log_offset_before = 0;
    {
        std::error_code ec;
        log_offset_before = static_cast<std::size_t>(fs::file_size(log_path, ec));
        if (ec) log_offset_before = 0;
    }

    // Run a trivial turn so AgentLoop rebuilds the per-turn schema array
    // and emits the `Tool manifest: ...` telemetry line.
    PromptResult r = h.prompt("Reply with only the word OK.");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());

    spdlog::default_logger()->flush();
    REQUIRE(fs::exists(log_path));
    std::ifstream in(log_path, std::ios::binary);
    REQUIRE(in.good());
    in.seekg(static_cast<std::streamoff>(log_offset_before));
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string tail = ss.str();

    INFO("integration_test.log tail length: " << tail.size());
    INFO("tail tail:\n" << tail.substr(tail.size() > 2000 ? tail.size() - 2000 : 0));

    // Manifest log line must be present.
    const auto manifest_pos = tail.find("Tool manifest:");
    REQUIRE(manifest_pos != std::string::npos);

    // Extract the token count from the most recent manifest line; the
    // lazy-mode collapse drops the per-turn schema cost dramatically vs.
    // the full-schema baseline (~3700t for ~15 tools on the default
    // capability matrix). 2000t is a comfortable threshold above the
    // measured ~900t but well below the full-schema floor.
    auto last_manifest = tail.rfind("Tool manifest:");
    REQUIRE(last_manifest != std::string::npos);
    auto tilde   = tail.find('~', last_manifest);
    auto t_token = tail.find('t', tilde);
    REQUIRE(tilde   != std::string::npos);
    REQUIRE(t_token != std::string::npos);
    int manifest_tokens = std::stoi(
        tail.substr(tilde + 1, t_token - tilde - 1));
    INFO("manifest tokens this run: " << manifest_tokens);
    REQUIRE(manifest_tokens < 2000);
}

TEST_CASE("toggling lazy_tool_manifest mid-session round-trips a basic turn",
          "[integration][llm][s6.11][lazy_manifest]")
{
    auto& h = harness();

    // Start with lazy ON, run a trivial turn.
    {
        LazyManifestGuard lazy(true);
        PromptResult r = h.prompt("Reply with only the word OK.");
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.errors.empty());
    }

    // Flip back to OFF (LazyManifestGuard restored it on dtor) and another
    // trivial turn should still work. The system prompt is rebuilt per turn
    // so toggling between turns is the supported path.
    {
        LazyManifestGuard lazy(false);
        PromptResult r = h.prompt("Reply with only the word OK.");
        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.errors.empty());
    }
}

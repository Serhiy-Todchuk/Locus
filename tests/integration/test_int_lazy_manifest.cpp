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

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

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

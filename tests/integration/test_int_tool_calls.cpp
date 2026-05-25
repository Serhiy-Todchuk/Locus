// S6.10 Tasks A + D -- tool-call robustness integration tests.
//
// Companion suite: tests/integration/test_int_small_model_robustness.cpp
// covers Tasks G / B / F / E / C from the same stage.
//
// Task A (JSON repair pre-pass) -- two cases:
//   1. Deterministic: feed malformed-JSON tool-call args through the harness's
//      actual ToolRegistry::parse_tool_call. Verifies the repair pre-pass is
//      wired through the production path (not just the unit-test fake).
//      Tagged [integration][s6.10] -- no live LLM round-trip needed.
//   2. Live regression: run a normal LLM tool-calling turn end-to-end and
//      verify it completes cleanly. Guards against Task A's catch-block
//      wiring breaking the happy path.
//
// Task D (server-side grammar-constrained decoding) -- one case:
//   Build a separate LMStudioClient with `grammar_mode = BestEffort` against
//   the same endpoint + model the harness is connected to, hand it the real
//   tool manifest, and verify a tool_call still comes back cleanly. LM Studio
//   honours `response_format: {type:"json_schema", ...}`; servers that don't
//   recognise the field ignore it. Either way the request succeeds.

#include "harness_fixture.h"

#include "llm/llm_client.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace locus::integration;
using namespace locus;

namespace {

std::string env_or_(const char* name, const std::string& fallback)
{
    const char* v = std::getenv(name);
    if (!v || std::strlen(v) == 0) return fallback;
    return v;
}

// Build an LLMConfig matching the harness's auto-detected endpoint + model
// with an extra `grammar_mode` override. Used by the Task D test to exercise
// the response_format path without disturbing the harness's shared client.
LLMConfig make_grammar_client_config(const IntegrationHarness& h, GrammarMode mode)
{
    LLMConfig cfg;
    cfg.base_url       = env_or_("LOCUS_INT_TEST_ENDPOINT", "http://127.0.0.1:1234");
    cfg.model          = h.model_id();
    cfg.temperature    = 0.2;
    cfg.max_tokens     = 512;
    cfg.context_limit  = 0;
    cfg.timeout_ms     = 120000;
    cfg.tool_format    = ToolFormat::Auto;
    cfg.grammar_mode   = mode;
    return cfg;
}

struct StreamRecorder {
    std::string                 text;
    std::vector<ToolCallRequest> tool_calls;
    std::vector<std::string>    errors;
    bool                        completed = false;

    StreamCallbacks make_callbacks()
    {
        StreamCallbacks cb;
        cb.on_token       = [this](const std::string& t) { text += t; };
        cb.on_tool_calls  = [this](const std::vector<ToolCallRequest>& v) {
            tool_calls = v;
        };
        cb.on_complete    = [this]() { completed = true; };
        cb.on_error       = [this](const std::string& e) { errors.push_back(e); };
        return cb;
    }
};

}  // namespace

// ---- Task A -- deterministic --------------------------------------------------

TEST_CASE("S6.10 Task A: ToolRegistry::parse_tool_call repairs malformed args "
          "through the live harness registry",
          "[integration][s6.10][json_repair]")
{
    // Use the real workspace's tool registry as the contextual integration
    // surface -- proves the repair pre-pass is wired through the production
    // factory path, not just the unit-test stand-in. The parse function is
    // static but the workspace + tool registry exist for free here, so any
    // future caller-side regression (e.g. a stray `arguments_json.empty()`
    // short-circuit before repair runs) would surface here too.
    auto& h = harness();
    (void) h.workspace();  // touch the harness so it builds at least once

    // Trailing comma + unquoted keys -- both Task A repair stages must fire.
    auto a = ToolRegistry::parse_tool_call(
        "call_int_a", "read_file",
        R"({path: "CLAUDE.md", offset: 1,})");
    REQUIRE(a.args["path"] == "CLAUDE.md");
    REQUIRE(a.args["offset"] == 1);

    // Surrounding prose + missing closer -- stage 6 (extract balanced) +
    // stage 4 (balance) compose.
    auto b = ToolRegistry::parse_tool_call(
        "call_int_b", "read_file",
        R"(Sure! Here's the call: {"path":"CLAUDE.md","offset":1)");
    REQUIRE(b.args["path"] == "CLAUDE.md");
    REQUIRE(b.args["offset"] == 1);

    // Field-name alias lift -- `arguments` wrapper transparently flattened.
    auto c = ToolRegistry::parse_tool_call(
        "call_int_c", "read_file",
        R"({"arguments":{"path":"CLAUDE.md","offset":1}})");
    REQUIRE(c.args["path"] == "CLAUDE.md");
    REQUIRE(c.args["offset"] == 1);
    REQUIRE_FALSE(c.args.contains("arguments"));
}

// ---- Task A -- live regression -----------------------------------------------

TEST_CASE("S6.10 Task A: normal LLM tool call still works end-to-end",
          "[integration][llm][s6.10][json_repair]")
{
    auto& h = harness();

    // Ask for a real tool call. The repair pre-pass should be inert (the LLM
    // typically emits valid JSON) -- this is a regression guard for Task A's
    // catch-block wiring around the normal happy path.
    PromptResult r = h.prompt(
        "Use the read_file tool to read the first 5 lines of CLAUDE.md.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("read_file"));

    const auto* res = r.find_result_for("read_file");
    REQUIRE(res != nullptr);
    REQUIRE_FALSE(res->display.empty());
}

// ---- Task D -- live grammar-constrained decoding -----------------------------

TEST_CASE("S6.10 Task D: tool call succeeds with grammar_mode=BestEffort",
          "[integration][llm][s6.10][grammar]")
{
    auto& h = harness();

    // Build a fresh client with grammar_mode flipped on. Same endpoint +
    // model as the harness; standalone so we don't disturb the shared agent's
    // conversation state.
    auto cfg    = make_grammar_client_config(h, GrammarMode::BestEffort);
    auto client = create_llm_client(cfg);

    // Mirror the real per-turn tool manifest so the LLM sees the exact shape
    // the agent ships -- this is what response_format constrains over.
    ToolRegistry reg;
    register_builtin_tools(reg);
    auto schema_arr = reg.build_schema_json(h.workspace(), ToolMode::agent,
                                            /*lazy=*/false);
    std::vector<ToolSchema> tools;
    for (auto& entry : schema_arr) {
        ToolSchema ts;
        ts.name        = entry["function"].value("name", "");
        ts.description = entry["function"].value("description", "");
        ts.parameters  = entry["function"]["parameters"];
        tools.push_back(std::move(ts));
    }
    REQUIRE_FALSE(tools.empty());

    std::vector<ChatMessage> msgs;
    {
        ChatMessage sys;
        sys.role    = MessageRole::system;
        sys.content =
            "You are a workspace assistant. When the user asks you to read a "
            "file, call the read_file tool with the right path. Do not answer "
            "from memory.";
        msgs.push_back(std::move(sys));

        ChatMessage user;
        user.role    = MessageRole::user;
        user.content =
            "Read CLAUDE.md (path \"CLAUDE.md\"), lines 1-5. Use the read_file "
            "tool.";
        msgs.push_back(std::move(user));
    }

    StreamRecorder rec;
    client->stream_completion(msgs, tools, rec.make_callbacks());

    REQUIRE(rec.completed);

    // Surface errors verbatim so a server that flat-out rejects response_format
    // (rare on LM Studio / llama-server, possible on Ollama variants) is
    // immediately recognisable from the test output.
    INFO("errors emitted: " << rec.errors.size());
    for (const auto& e : rec.errors) INFO("  - " << e);

    // The headline assertion: a tool call comes back well-formed under the
    // grammar-constrained path. The schema enforces `{name, arguments}` per
    // call; if response_format was honoured the tool_calls array IS non-empty
    // and the per-call shape IS valid.
    REQUIRE_FALSE(rec.tool_calls.empty());
    const auto& tc = rec.tool_calls.front();
    REQUIRE(tc.name == "read_file");
    REQUIRE_FALSE(tc.arguments.empty());

    // The arguments string should parse cleanly as JSON -- the whole point
    // of Task D is that even when the model wants to be sloppy, the sampler
    // (when the server honours the schema) forces well-formed JSON. Servers
    // that don't honour it fall through to Task A's repair, which we assert
    // separately above.
    auto parsed = nlohmann::json::parse(tc.arguments, nullptr, /*allow_exceptions=*/false);
    INFO("arguments raw: " << tc.arguments);
    REQUIRE_FALSE(parsed.is_discarded());
    REQUIRE(parsed.is_object());
    REQUIRE(parsed.contains("path"));
}

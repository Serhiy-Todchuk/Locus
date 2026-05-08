// S4.U -- stream-decode completeness + max_tokens discipline.
//
// Verifies the LMStudioClient surfaces `finish_reason="length"` as a clear
// `on_error` warning AND drops malformed tool calls (empty / unparseable
// `arguments`) before they leak into history. Going through the full agent
// loop is awkward because the harness LLM client is built with the standard
// max_tokens; for this test we instantiate a private LLMClient with a tiny
// max_tokens and drive it directly via stream_completion. The agent path
// is what we're guarding -- but the bug has always lived inside the
// LMStudioClient layer, so testing there is both sufficient and faster.

#include "harness_fixture.h"

#include "llm/llm_client.h"
#include "tools/tools.h"
#include "tools/tool_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace locus::integration;
using namespace locus;

namespace {

// Honor the same env vars as the main harness so the test reaches whatever
// LM Studio endpoint / model the user has configured.
std::string env_or_(const char* name, const std::string& fallback)
{
    const char* v = std::getenv(name);
    if (!v || std::strlen(v) == 0) return fallback;
    return v;
}

std::unique_ptr<ILLMClient> make_constrained_client(const IntegrationHarness& h,
                                                    int max_tokens)
{
    LLMConfig cfg;
    cfg.base_url    = env_or_("LOCUS_INT_TEST_ENDPOINT", "http://127.0.0.1:1234");
    cfg.model       = h.model_id();      // already auto-detected by the harness
    cfg.temperature = 0.2;
    cfg.max_tokens  = max_tokens;
    cfg.context_limit = 0;               // not used for stream_completion
    cfg.timeout_ms  = 120000;
    return create_llm_client(std::move(cfg));
}

struct StreamRecorder {
    std::string                 text;
    std::string                 reasoning;
    std::vector<ToolCallRequest> tool_calls;
    std::vector<std::string>    errors;
    bool                        completed = false;
    CompletionUsage             usage{};

    StreamCallbacks make_callbacks()
    {
        StreamCallbacks cb;
        cb.on_token             = [this](const std::string& t) { text += t; };
        cb.on_reasoning_token   = [this](const std::string& t) { reasoning += t; };
        cb.on_tool_calls        = [this](const std::vector<ToolCallRequest>& v) {
            tool_calls = v;
        };
        cb.on_complete          = [this]() { completed = true; };
        cb.on_error             = [this](const std::string& e) { errors.push_back(e); };
        cb.on_usage             = [this](const CompletionUsage& u) { usage = u; };
        return cb;
    }
};

bool any_error_mentions(const std::vector<std::string>& errs, std::string_view needle)
{
    for (const auto& e : errs)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("LMStudioClient surfaces finish_reason=length as a clear on_error",
          "[integration][llm][max_tokens]")
{
    auto& h = harness();

    // 200 tokens is small enough to truncate any non-trivial generation, big
    // enough that the model gets to start producing visible text first (so we
    // can confirm we're not erroring at zero-output).
    auto client = make_constrained_client(h, /*max_tokens=*/200);

    std::vector<ChatMessage> msgs;
    {
        ChatMessage sys;
        sys.role    = MessageRole::system;
        sys.content =
            "You are a verbose assistant. When asked to write text, produce a "
            "long response without stopping early.";
        msgs.push_back(std::move(sys));

        ChatMessage user;
        user.role    = MessageRole::user;
        user.content =
            "Write a continuous prose explanation of why context-free grammars "
            "are useful for language tooling. Aim for at least 1000 words. "
            "Do not summarize, do not stop early, do not bullet -- write a "
            "single long flowing prose paragraph.";
        msgs.push_back(std::move(user));
    }

    StreamRecorder rec;
    client->stream_completion(msgs, /*tools=*/{}, rec.make_callbacks());

    // Stream finished cleanly (no transport failure) and we got SOME text
    // before being cut off.
    REQUIRE(rec.completed);
    REQUIRE_FALSE(rec.text.empty());

    // The fix: an `on_error` MUST fire with a recognisable truncation
    // message. The exact phrasing is verbose so the user can copy-paste
    // into config; we just key on the parts that are stable.
    INFO("errors emitted: " << rec.errors.size());
    for (const auto& e : rec.errors) INFO("  - " << e);
    REQUIRE(any_error_mentions(rec.errors, "max_tokens"));

    // Server-reported usage must report exactly 200 completion tokens (the
    // cap we set) so we know the truncation is what fired the warning.
    REQUIRE(rec.usage.completion_tokens == 200);
}

TEST_CASE("LMStudioClient drops empty-argument tool calls without poisoning history",
          "[integration][llm][max_tokens][tool_call_validation]")
{
    auto& h = harness();

    // Cap small enough that asking for a big multi-tool plan will likely
    // truncate either the tool-call arguments or the visible-text envelope
    // around them. This is the exact failure mode that produced HTTP 500
    // on the next round in the original bug report.
    auto client = make_constrained_client(h, /*max_tokens=*/120);

    // Use the real tool manifest so the model has real `write_file` schemas
    // to invoke. Building one by hand would diverge from production.
    ToolRegistry reg;
    register_builtin_tools(reg);
    auto schema_arr = reg.build_schema_json(h.workspace(), ToolMode::agent);
    std::vector<ToolSchema> tools;
    for (auto& entry : schema_arr) {
        if (!entry.contains("function")) continue;
        auto& fn = entry["function"];
        ToolSchema ts;
        ts.name        = fn.value("name", "");
        ts.description = fn.value("description", "");
        ts.parameters  = fn.value("parameters", nlohmann::json::object());
        tools.push_back(std::move(ts));
    }
    REQUIRE_FALSE(tools.empty());

    std::vector<ChatMessage> msgs;
    {
        ChatMessage sys;
        sys.role    = MessageRole::system;
        sys.content =
            "You are an autonomous coding agent. When asked to scaffold a "
            "project, immediately call write_file MULTIPLE times in one turn "
            "for each file. Each file must contain at least 200 bytes of "
            "boilerplate content.";
        msgs.push_back(std::move(sys));

        ChatMessage user;
        user.role    = MessageRole::user;
        user.content =
            "Scaffold a tiny WebGPU project under tests/integration_tmp/. "
            "Call write_file for each of these paths in this single turn: "
            "tests/integration_tmp/index.html, "
            "tests/integration_tmp/src/main.js, "
            "tests/integration_tmp/src/scene.js, "
            "tests/integration_tmp/src/renderer.js. "
            "Each file's content must be a complete, realistic stub of at "
            "least 200 bytes. Issue all four calls in one assistant turn.";
        msgs.push_back(std::move(user));
    }

    StreamRecorder rec;
    client->stream_completion(msgs, tools, rec.make_callbacks());

    REQUIRE(rec.completed);

    // Every delivered tool call MUST have non-empty + parseable arguments.
    // The whole point of S4.U's drop logic is to ensure no caller ever sees
    // an arguments="" entry. We don't care how many calls survive -- 0, 1,
    // 2, all 4 -- only that none of the survivors are malformed.
    for (const auto& tc : rec.tool_calls) {
        INFO("delivered tool: " << tc.name << " id=" << tc.id
             << " args=" << tc.arguments);
        REQUIRE_FALSE(tc.name.empty());
        REQUIRE_FALSE(tc.arguments.empty());
        // Parse must succeed.
        REQUIRE_NOTHROW(nlohmann::json::parse(tc.arguments));
    }
}

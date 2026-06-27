// S6.14 -- thinking ON/OFF/auto, live-LLM assertion.
//
// With thinking forced Off on a Qwen 3.x model, a prompt that normally elicits
// a visible reasoning stream must produce ZERO reasoning_content chunks. We
// drive a private LMStudioClient directly (same approach as the [max_tokens]
// test) rather than going through the agent loop, because the knob lives on
// LLMConfig and is baked into the client at construction -- a private client
// lets us flip it per-case without rebuilding the shared harness.
//
// SKIPS when the loaded model isn't in a family with a known no-think
// mechanism (Qwen 3.x is the canonical one). On a non-Qwen-3.x model the Off
// request is a no-op, so asserting "no reasoning" would be a false negative.

#include "harness_fixture.h"

#include "llm/llm_client.h"
#include "llm/thinking_injection.h"

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

std::unique_ptr<ILLMClient> make_thinking_client(const IntegrationHarness& h,
                                                 ThinkingMode mode)
{
    LLMConfig cfg;
    cfg.base_url        = env_or_("LOCUS_INT_TEST_ENDPOINT", "http://127.0.0.1:1234");
    cfg.model           = h.model_id();
    cfg.temperature     = 0.2;
    cfg.max_tokens      = 1024;
    cfg.context_limit   = 0;
    cfg.timeout_ms      = 180000;
    cfg.enable_thinking = mode;
    return create_llm_client(std::move(cfg));
}

struct StreamRecorder {
    std::string                  text;
    std::string                  reasoning;
    std::vector<std::string>     errors;
    bool                         completed = false;

    StreamCallbacks make_callbacks()
    {
        StreamCallbacks cb;
        cb.on_token           = [this](const std::string& t) { text += t; };
        cb.on_reasoning_token = [this](const std::string& t) { reasoning += t; };
        cb.on_complete        = [this]() { completed = true; };
        cb.on_error           = [this](const std::string& e) { errors.push_back(e); };
        return cb;
    }
};

std::vector<ChatMessage> thinky_prompt()
{
    std::vector<ChatMessage> msgs;
    ChatMessage sys;
    sys.role    = MessageRole::system;
    sys.content = "You are a careful reasoning assistant.";
    msgs.push_back(std::move(sys));

    ChatMessage user;
    user.role    = MessageRole::user;
    user.content =
        "A train leaves city A at 60 km/h and another leaves city B (300 km "
        "away) at 40 km/h toward each other at the same time. After how many "
        "hours do they meet? Think it through step by step.";
    msgs.push_back(std::move(user));
    return msgs;
}

}  // namespace

TEST_CASE("thinking Off suppresses reasoning_content on a Qwen 3.x model",
          "[integration][llm][s6.14]")
{
    auto& h = harness();

    if (classify_thinking_family(h.model_id()) != ThinkingFamily::Qwen3Hybrid) {
        WARN("loaded model '" + h.model_id() +
             "' is not Qwen 3.x hybrid; skipping (no-think no-op would "
             "false-pass)");
        return;
    }

    auto client = make_thinking_client(h, ThinkingMode::Off);

    StreamRecorder rec;
    client->stream_completion(thinky_prompt(), /*tools=*/{}, rec.make_callbacks());

    REQUIRE(rec.completed);
    // The whole point: NO reasoning channel bytes when thinking is forced off.
    INFO("reasoning bytes=" << rec.reasoning.size()
         << " text bytes=" << rec.text.size());
    REQUIRE(rec.reasoning.empty());
    // It still answered (some visible text came back).
    REQUIRE_FALSE(rec.text.empty());
}

TEST_CASE("thinking On produces reasoning_content on a Qwen 3.x model",
          "[integration][llm][s6.14]")
{
    auto& h = harness();

    if (classify_thinking_family(h.model_id()) != ThinkingFamily::Qwen3Hybrid) {
        WARN("loaded model '" + h.model_id() +
             "' is not Qwen 3.x hybrid; skipping (cannot assert reasoning is "
             "present)");
        return;
    }

    auto client = make_thinking_client(h, ThinkingMode::On);

    StreamRecorder rec;
    client->stream_completion(thinky_prompt(), /*tools=*/{}, rec.make_callbacks());

    REQUIRE(rec.completed);
    INFO("reasoning bytes=" << rec.reasoning.size()
         << " text bytes=" << rec.text.size());
    // Forcing thinking on must surface a reasoning stream. This is the positive
    // control that proves the Off case above is the flag's doing, not a model
    // that simply never reasons.
    REQUIRE_FALSE(rec.reasoning.empty());
}

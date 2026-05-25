// S6.10 Task C -- past-turn reasoning_content stripping tests.
//
// The strip happens inside AgentLoop::run_step right before the
// stream_completion call. The unit test surface here covers:
//   - ChatMessage round-trips reasoning_content via to_json / from_json
//   - ConversationHistory carries it through save/load
// Behavioural coverage of the strip pass (mid-decision-chain rule, the
// only-reasoning edge case, the feature flag) is exercised at the unit
// level by re-implementing the strip logic in this file. That is the same
// shape as AgentLoop's branch -- copying it here avoids spinning up a real
// LLM client, while the integration test in test_int_tool_calls.cpp covers
// the live AgentLoop path end-to-end.

#include "agent/conversation.h"
#include "llm/llm_client.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using locus::ChatMessage;
using locus::ConversationHistory;
using locus::MessageRole;

namespace {

// Strip pass mirroring AgentLoop's branch, parameterised by the flag.
std::vector<ChatMessage> apply_strip(std::vector<ChatMessage> msgs,
                                      bool strip_enabled)
{
    if (!strip_enabled) return msgs;
    int last_assistant_idx = -1;
    for (int i = static_cast<int>(msgs.size()) - 1; i >= 0; --i) {
        if (msgs[i].role == MessageRole::assistant) {
            last_assistant_idx = i;
            break;
        }
    }
    bool keep_latest = !msgs.empty() &&
                       msgs.back().role == MessageRole::tool;
    for (int i = 0; i < static_cast<int>(msgs.size()); ++i) {
        auto& m = msgs[i];
        if (m.role != MessageRole::assistant) continue;
        if (m.reasoning_content.empty()) continue;
        if (m.content.empty() && m.tool_calls.empty()) continue; // only-reasoning
        if (i == last_assistant_idx && keep_latest) continue;
        m.reasoning_content.clear();
    }
    return msgs;
}

ChatMessage make_assistant(std::string content, std::string reasoning)
{
    ChatMessage m;
    m.role = MessageRole::assistant;
    m.content = std::move(content);
    m.reasoning_content = std::move(reasoning);
    return m;
}

ChatMessage make_user(std::string content)
{
    ChatMessage m;
    m.role = MessageRole::user;
    m.content = std::move(content);
    return m;
}

ChatMessage make_tool_result(std::string content)
{
    ChatMessage m;
    m.role = MessageRole::tool;
    m.content = std::move(content);
    m.tool_call_id = "c1";
    return m;
}

} // namespace

TEST_CASE("ChatMessage: reasoning_content round-trips through to_json/from_json",
          "[s6.10][thinking_strip]")
{
    ChatMessage m = make_assistant("ok", "step 1: think; step 2: act");
    auto j = m.to_json();
    REQUIRE(j.contains("reasoning_content"));
    REQUIRE(j["reasoning_content"] == "step 1: think; step 2: act");

    auto m2 = ChatMessage::from_json(j);
    REQUIRE(m2.reasoning_content == m.reasoning_content);
    REQUIRE(m2.content == m.content);
}

TEST_CASE("ChatMessage: empty reasoning_content is omitted from JSON",
          "[s6.10][thinking_strip]")
{
    ChatMessage m = make_assistant("just text", "");
    auto j = m.to_json();
    REQUIRE_FALSE(j.contains("reasoning_content"));
}

TEST_CASE("ConversationHistory: reasoning_content survives save+load",
          "[s6.10][thinking_strip]")
{
    ConversationHistory h;
    h.add(make_user("hi"));
    h.add(make_assistant("hello", "long reasoning block here"));

    auto j = h.to_json();
    auto h2 = ConversationHistory::from_json(j);

    REQUIRE(h2.size() == 2);
    const auto& assistant = h2.messages()[1];
    REQUIRE(assistant.role == MessageRole::assistant);
    REQUIRE(assistant.reasoning_content == "long reasoning block here");
}

TEST_CASE("strip pass: 3-turn history strips reasoning from all but latest",
          "[s6.10][thinking_strip]")
{
    std::vector<ChatMessage> hist;
    hist.push_back(make_user("u1"));
    hist.push_back(make_assistant("a1", "reasoning1"));
    hist.push_back(make_user("u2"));
    hist.push_back(make_assistant("a2", "reasoning2"));
    hist.push_back(make_user("u3"));
    hist.push_back(make_assistant("a3", "reasoning3"));

    // No tool message at the end -- so the keep_latest exception does NOT
    // apply: every past-turn assistant including the last gets stripped.
    auto stripped = apply_strip(hist, /*strip_enabled=*/true);
    REQUIRE(stripped[1].reasoning_content.empty());
    REQUIRE(stripped[3].reasoning_content.empty());
    REQUIRE(stripped[5].reasoning_content.empty());
}

TEST_CASE("strip pass: most-recent assistant kept when followed by tool result",
          "[s6.10][thinking_strip]")
{
    std::vector<ChatMessage> hist;
    hist.push_back(make_user("u1"));
    hist.push_back(make_assistant("a1", "older reasoning"));
    hist.push_back(make_user("u2"));
    // Assistant emitted a tool call; latest message in history is the tool
    // result -- we're mid-decision-chain, so keep the latest reasoning.
    hist.push_back(make_assistant("calling tool", "current decision reasoning"));
    hist.push_back(make_tool_result("tool returned ok"));

    auto stripped = apply_strip(hist, /*strip_enabled=*/true);
    REQUIRE(stripped[1].reasoning_content.empty());  // past turn stripped
    REQUIRE(stripped[3].reasoning_content ==
            "current decision reasoning");           // latest preserved
}

TEST_CASE("strip pass: assistant with ONLY reasoning is preserved",
          "[s6.10][thinking_strip]")
{
    std::vector<ChatMessage> hist;
    hist.push_back(make_user("u1"));
    // Assistant emitted only reasoning (no text, no tool calls). The
    // post-S6.13 edge case the spec calls out: keep it intact rather than
    // stripping the message into a phantom-empty bubble.
    ChatMessage only_reasoning;
    only_reasoning.role = MessageRole::assistant;
    only_reasoning.reasoning_content = "thinking out loud only";
    hist.push_back(only_reasoning);
    hist.push_back(make_user("u2"));
    hist.push_back(make_assistant("response", "reasoning2"));

    auto stripped = apply_strip(hist, /*strip_enabled=*/true);
    // The only-reasoning message stays untouched.
    REQUIRE(stripped[1].reasoning_content == "thinking out loud only");
    // The normal assistant with content is stripped (it's not the latest
    // before a tool result).
    REQUIRE(stripped[3].reasoning_content.empty());
}

TEST_CASE("strip pass: feature flag off preserves all reasoning",
          "[s6.10][thinking_strip]")
{
    std::vector<ChatMessage> hist;
    hist.push_back(make_user("u1"));
    hist.push_back(make_assistant("a1", "reasoning1"));
    hist.push_back(make_user("u2"));
    hist.push_back(make_assistant("a2", "reasoning2"));

    auto kept = apply_strip(hist, /*strip_enabled=*/false);
    REQUIRE(kept[1].reasoning_content == "reasoning1");
    REQUIRE(kept[3].reasoning_content == "reasoning2");
}

TEST_CASE("strip pass: user/system/tool messages are never modified",
          "[s6.10][thinking_strip]")
{
    std::vector<ChatMessage> hist;
    ChatMessage sys;
    sys.role = MessageRole::system;
    sys.content = "system text";
    hist.push_back(sys);
    hist.push_back(make_user("u1"));
    hist.push_back(make_assistant("a1", "reasoning"));

    auto stripped = apply_strip(hist, /*strip_enabled=*/true);
    REQUIRE(stripped[0].content == "system text");
    REQUIRE(stripped[1].content == "u1");
    REQUIRE(stripped[2].reasoning_content.empty());
}

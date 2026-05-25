// S6.10 Task B -- QualityMonitor detector tests.
//
// Pure-function evaluation; the AgentCore wiring (nudge injection + 2-cap
// shared with the reasoning watchdog) is covered by the agent-loop
// integration test set, not here.

#include "agent/quality_monitor.h"
#include "llm/llm_client.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using locus::ChatMessage;
using locus::MessageRole;
using locus::QualityCorrection;
using locus::QualityCorrectionKind;
using locus::QualityMonitor;
using locus::ToolCallRequest;

namespace {

ChatMessage make_user(std::string content)
{
    ChatMessage m;
    m.role = MessageRole::user;
    m.content = std::move(content);
    return m;
}

ChatMessage make_assistant(std::string content,
                            std::string reasoning = "")
{
    ChatMessage m;
    m.role = MessageRole::assistant;
    m.content = std::move(content);
    m.reasoning_content = std::move(reasoning);
    return m;
}

ChatMessage make_assistant_tool_call(std::string name,
                                      std::string args_json,
                                      std::string id = "c1")
{
    ChatMessage m;
    m.role = MessageRole::assistant;
    ToolCallRequest tc;
    tc.id        = std::move(id);
    tc.name      = std::move(name);
    tc.arguments = std::move(args_json);
    m.tool_calls.push_back(std::move(tc));
    return m;
}

ChatMessage make_tool_result(std::string id, std::string content)
{
    ChatMessage m;
    m.role = MessageRole::tool;
    m.tool_call_id = std::move(id);
    m.content = std::move(content);
    return m;
}

} // namespace

TEST_CASE("QualityMonitor: empty history -> no correction",
          "[s6.10][quality]")
{
    std::vector<ChatMessage> msgs;
    REQUIRE_FALSE(QualityMonitor::evaluate(msgs).has_value());
}

TEST_CASE("QualityMonitor: healthy turn -> no correction",
          "[s6.10][quality]")
{
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("Hi"));
    msgs.push_back(make_assistant("Hello there!"));
    REQUIRE_FALSE(QualityMonitor::evaluate(msgs).has_value());
}

TEST_CASE("QualityMonitor: empty_response fires when nothing is returned",
          "[s6.10][quality]")
{
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("Hi"));
    msgs.push_back(make_assistant(""));  // no content, no tool, no reasoning

    auto c = QualityMonitor::evaluate(msgs);
    REQUIRE(c.has_value());
    REQUIRE(c->kind == QualityCorrectionKind::empty_response);
    REQUIRE_FALSE(c->corrective_message.empty());
}

TEST_CASE("QualityMonitor: reasoning-only assistant is NOT empty_response",
          "[s6.10][quality]")
{
    // S6.13 watchdog is the right surface for reasoning-only assistants;
    // the quality monitor must not double-fire on them.
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("Hi"));
    msgs.push_back(make_assistant("", "thinking only"));
    REQUIRE_FALSE(QualityMonitor::evaluate(msgs).has_value());
}

TEST_CASE("QualityMonitor: repeated_tool_call fires on identical name+args",
          "[s6.10][quality]")
{
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("read foo"));
    msgs.push_back(make_assistant_tool_call("read_file", R"({"path":"foo"})", "c1"));
    msgs.push_back(make_tool_result("c1", "...file content..."));
    msgs.push_back(make_assistant_tool_call("read_file", R"({"path":"foo"})", "c2"));

    auto c = QualityMonitor::evaluate(msgs);
    REQUIRE(c.has_value());
    REQUIRE(c->kind == QualityCorrectionKind::repeated_tool_call);
    REQUIRE(c->corrective_message.find("read_file") != std::string::npos);
}

TEST_CASE("QualityMonitor: different tool name -> not a repeat",
          "[s6.10][quality]")
{
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("look"));
    msgs.push_back(make_assistant_tool_call("read_file", R"({"path":"foo"})", "c1"));
    msgs.push_back(make_tool_result("c1", "..."));
    msgs.push_back(make_assistant_tool_call("write_file", R"({"path":"foo"})", "c2"));

    REQUIRE_FALSE(QualityMonitor::evaluate(msgs).has_value());
}

TEST_CASE("QualityMonitor: different args -> not a repeat",
          "[s6.10][quality]")
{
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("look"));
    msgs.push_back(make_assistant_tool_call("read_file", R"({"path":"foo"})", "c1"));
    msgs.push_back(make_tool_result("c1", "..."));
    msgs.push_back(make_assistant_tool_call("read_file", R"({"path":"bar"})", "c2"));

    REQUIRE_FALSE(QualityMonitor::evaluate(msgs).has_value());
}

TEST_CASE("QualityMonitor: args order-insensitive equality detects repeats",
          "[s6.10][quality]")
{
    // Canonical-JSON comparison: same key/value pairs in different order
    // is still a repeat.
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("look"));
    msgs.push_back(make_assistant_tool_call("search",
        R"({"mode":"text","query":"foo"})", "c1"));
    msgs.push_back(make_tool_result("c1", "no results"));
    msgs.push_back(make_assistant_tool_call("search",
        R"({"query":"foo","mode":"text"})", "c2"));

    auto c = QualityMonitor::evaluate(msgs);
    REQUIRE(c.has_value());
    REQUIRE(c->kind == QualityCorrectionKind::repeated_tool_call);
}

TEST_CASE("QualityMonitor: first tool call -> no repeat detection",
          "[s6.10][quality]")
{
    // Only one assistant tool call in history -- can't be a repeat of
    // anything yet.
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("look"));
    msgs.push_back(make_assistant_tool_call("read_file", R"({"path":"foo"})", "c1"));

    REQUIRE_FALSE(QualityMonitor::evaluate(msgs).has_value());
}

TEST_CASE("QualityMonitor: previous assistant without tool_calls breaks chain",
          "[s6.10][quality]")
{
    // A repeat is only counted when the immediately-previous assistant
    // message also called a tool. A user message between two tool calls
    // is treated as a fresh start by the detector.
    std::vector<ChatMessage> msgs;
    msgs.push_back(make_user("look"));
    msgs.push_back(make_assistant_tool_call("read_file", R"({"path":"foo"})", "c1"));
    msgs.push_back(make_tool_result("c1", "..."));
    // Plain assistant text message (no tool call)
    msgs.push_back(make_assistant("Let me check that file."));
    msgs.push_back(make_user("ok"));
    msgs.push_back(make_assistant_tool_call("read_file", R"({"path":"foo"})", "c2"));

    // The walk skips the plain-text assistant and would compare to the
    // previous tool-call assistant; this IS a repeat. (Spec: "look back 1
    // tool-call turn (strict)" -- the plain-text message doesn't count.)
    auto c = QualityMonitor::evaluate(msgs);
    REQUIRE(c.has_value());
    REQUIRE(c->kind == QualityCorrectionKind::repeated_tool_call);
}

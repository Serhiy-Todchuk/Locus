#include <catch2/catch_test_macros.hpp>

#include "llm/llm_client.h"
#include "llm/sse_parser.h"
#include "llm/token_counter.h"

#include <string>
#include <vector>

using namespace locus;

// ============================================================================
// SSE Parser
// ============================================================================

TEST_CASE("SseParser: single complete event", "[s0.5]")
{
    std::vector<std::string> payloads;
    SseParser parser([&](const std::string& data) -> bool {
        payloads.push_back(data);
        return true;
    });

    parser.feed("data: hello world\n\n");
    parser.finish();

    REQUIRE(payloads.size() == 1);
    CHECK(payloads[0] == "hello world");
}

TEST_CASE("SseParser: multiple events in one chunk", "[s0.5]")
{
    std::vector<std::string> payloads;
    SseParser parser([&](const std::string& data) -> bool {
        payloads.push_back(data);
        return true;
    });

    parser.feed("data: first\ndata: second\ndata: third\n");
    parser.finish();

    REQUIRE(payloads.size() == 3);
    CHECK(payloads[0] == "first");
    CHECK(payloads[1] == "second");
    CHECK(payloads[2] == "third");
}

TEST_CASE("SseParser: data split across chunks", "[s0.5]")
{
    std::vector<std::string> payloads;
    SseParser parser([&](const std::string& data) -> bool {
        payloads.push_back(data);
        return true;
    });

    parser.feed("dat");
    parser.feed("a: sp");
    parser.feed("lit\n");
    parser.finish();

    REQUIRE(payloads.size() == 1);
    CHECK(payloads[0] == "split");
}

TEST_CASE("SseParser: handles CRLF line endings", "[s0.5]")
{
    std::vector<std::string> payloads;
    SseParser parser([&](const std::string& data) -> bool {
        payloads.push_back(data);
        return true;
    });

    parser.feed("data: crlf\r\ndata: next\r\n");
    parser.finish();

    REQUIRE(payloads.size() == 2);
    CHECK(payloads[0] == "crlf");
    CHECK(payloads[1] == "next");
}

TEST_CASE("SseParser: ignores comments and empty lines", "[s0.5]")
{
    std::vector<std::string> payloads;
    SseParser parser([&](const std::string& data) -> bool {
        payloads.push_back(data);
        return true;
    });

    parser.feed(": this is a comment\n\ndata: real\n\n");
    parser.finish();

    REQUIRE(payloads.size() == 1);
    CHECK(payloads[0] == "real");
}

TEST_CASE("SseParser: [DONE] is passed through as data", "[s0.5]")
{
    std::vector<std::string> payloads;
    SseParser parser([&](const std::string& data) -> bool {
        payloads.push_back(data);
        return true;
    });

    parser.feed("data: {\"text\":\"hi\"}\ndata: [DONE]\n");
    parser.finish();

    REQUIRE(payloads.size() == 2);
    CHECK(payloads[0] == "{\"text\":\"hi\"}");
    CHECK(payloads[1] == "[DONE]");
}

TEST_CASE("SseParser: partial line flushed on finish", "[s0.5]")
{
    std::vector<std::string> payloads;
    SseParser parser([&](const std::string& data) -> bool {
        payloads.push_back(data);
        return true;
    });

    // No trailing newline — must be flushed by finish().
    parser.feed("data: unterminated");
    parser.finish();

    REQUIRE(payloads.size() == 1);
    CHECK(payloads[0] == "unterminated");
}

// ============================================================================
// ChatMessage JSON round-trip
// ============================================================================

TEST_CASE("ChatMessage: user message round-trip", "[s0.5]")
{
    ChatMessage m;
    m.role = MessageRole::user;
    m.content = "Hello, world!";

    auto j = m.to_json();
    CHECK(j["role"] == "user");
    CHECK(j["content"] == "Hello, world!");

    auto m2 = ChatMessage::from_json(j);
    CHECK(m2.role == MessageRole::user);
    CHECK(m2.content == "Hello, world!");
}

TEST_CASE("ChatMessage: assistant message with tool calls", "[s0.5]")
{
    ChatMessage m;
    m.role = MessageRole::assistant;
    m.content = "";
    m.tool_calls = {{
        "call_123", "read_file", R"({"path":"src/main.cpp","offset":0,"length":50})"
    }};

    auto j = m.to_json();
    REQUIRE(j.contains("tool_calls"));
    REQUIRE(j["tool_calls"].size() == 1);
    CHECK(j["tool_calls"][0]["id"] == "call_123");
    CHECK(j["tool_calls"][0]["function"]["name"] == "read_file");

    auto m2 = ChatMessage::from_json(j);
    REQUIRE(m2.tool_calls.size() == 1);
    CHECK(m2.tool_calls[0].name == "read_file");
    CHECK(m2.tool_calls[0].id == "call_123");
}

TEST_CASE("ChatMessage: tool result message", "[s0.5]")
{
    ChatMessage m;
    m.role = MessageRole::tool;
    m.content = "File contents here...";
    m.tool_call_id = "call_123";

    auto j = m.to_json();
    CHECK(j["role"] == "tool");
    CHECK(j["tool_call_id"] == "call_123");

    auto m2 = ChatMessage::from_json(j);
    CHECK(m2.role == MessageRole::tool);
    CHECK(m2.tool_call_id == "call_123");
}

// ============================================================================
// Token estimation
// ============================================================================

TEST_CASE("Token estimation: basic heuristic", "[s0.5]")
{
    // Empty string = 0 tokens (rounds to 0 with (0+3)/4 = 0).
    // Actually (0+3)/4 = 0 in integer division.
    CHECK(TokenCounter::estimate("") == 0);

    // 4 chars = 1 token
    CHECK(TokenCounter::estimate("abcd") == 1);

    // 5 chars = 2 tokens
    CHECK(TokenCounter::estimate("abcde") == 2);

    // 100 chars = 25 tokens
    std::string hundred(100, 'x');
    CHECK(TokenCounter::estimate(hundred) == 25);
}

TEST_CASE("Token estimation: conversation", "[s0.5]")
{
    std::vector<ChatMessage> msgs;
    msgs.push_back({MessageRole::system, "You are helpful."});
    msgs.push_back({MessageRole::user,   "Hello"});

    int tokens = TokenCounter::estimate(msgs);
    // system: 4 overhead + 16/4=4 = 8
    // user:   4 overhead + 5/4=1 (rounds: (5+3)/4=2) = 6
    // total = 14
    CHECK(tokens > 0);
    CHECK(tokens < 50);  // sanity — not wildly off
}

// ============================================================================
// Tool-call detection from simulated SSE stream
// ============================================================================

TEST_CASE("Tool-call accumulation from SSE chunks", "[s0.5]")
{
    // Simulate what LMStudioClient's SSE callback does internally.
    // We rebuild the accumulation logic here to test it in isolation.

    struct ToolCallAccum {
        std::string id;
        std::string name;
        std::string arguments;
    };
    std::vector<ToolCallAccum> accum;

    // Simulated SSE data chunks (as sent by OpenAI-compatible servers).
    std::vector<std::string> chunks = {
        // First chunk: tool call start with id and partial function name
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_abc","function":{"name":"read_","arguments":""}}]},"finish_reason":null}]})",
        // Second chunk: more of the function name
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"name":"file","arguments":""}}]},"finish_reason":null}]})",
        // Third chunk: arguments streamed
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"path\":"}}]},"finish_reason":null}]})",
        // Fourth chunk: more arguments
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"main.cpp\"}"}}]},"finish_reason":null}]})",
        // Final chunk with finish_reason
        R"({"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})",
    };

    for (auto& chunk_str : chunks) {
        auto delta_json = nlohmann::json::parse(chunk_str);
        auto& choice = delta_json["choices"][0];
        auto& delta = choice["delta"];

        if (delta.contains("tool_calls")) {
            for (auto& tc_delta : delta["tool_calls"]) {
                int idx = tc_delta.value("index", 0);
                while (static_cast<int>(accum.size()) <= idx)
                    accum.push_back({});

                auto& a = accum[idx];
                if (tc_delta.contains("id") && !tc_delta["id"].is_null())
                    a.id = tc_delta["id"].get<std::string>();
                if (tc_delta.contains("function")) {
                    auto& fn = tc_delta["function"];
                    if (fn.contains("name") && !fn["name"].is_null())
                        a.name += fn["name"].get<std::string>();
                    if (fn.contains("arguments") && !fn["arguments"].is_null())
                        a.arguments += fn["arguments"].get<std::string>();
                }
            }
        }
    }

    REQUIRE(accum.size() == 1);
    CHECK(accum[0].id == "call_abc");
    CHECK(accum[0].name == "read_file");
    CHECK(accum[0].arguments == R"({"path":"main.cpp"})");
}

TEST_CASE("Multiple tool calls accumulated", "[s0.5]")
{
    struct ToolCallAccum {
        std::string id;
        std::string name;
        std::string arguments;
    };
    std::vector<ToolCallAccum> accum;

    // Two tool calls in one response (different indices).
    std::vector<std::string> chunks = {
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"search_text","arguments":"{\"query\":\"foo\"}"}}]},"finish_reason":null}]})",
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":1,"id":"call_2","function":{"name":"read_file","arguments":"{\"path\":\"a.cpp\"}"}}]},"finish_reason":null}]})",
        R"({"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})",
    };

    for (auto& chunk_str : chunks) {
        auto delta_json = nlohmann::json::parse(chunk_str);
        auto& delta = delta_json["choices"][0]["delta"];

        if (delta.contains("tool_calls")) {
            for (auto& tc_delta : delta["tool_calls"]) {
                int idx = tc_delta.value("index", 0);
                while (static_cast<int>(accum.size()) <= idx)
                    accum.push_back({});

                auto& a = accum[idx];
                if (tc_delta.contains("id") && !tc_delta["id"].is_null())
                    a.id = tc_delta["id"].get<std::string>();
                if (tc_delta.contains("function")) {
                    auto& fn = tc_delta["function"];
                    if (fn.contains("name") && !fn["name"].is_null())
                        a.name += fn["name"].get<std::string>();
                    if (fn.contains("arguments") && !fn["arguments"].is_null())
                        a.arguments += fn["arguments"].get<std::string>();
                }
            }
        }
    }

    REQUIRE(accum.size() == 2);
    CHECK(accum[0].id == "call_1");
    CHECK(accum[0].name == "search_text");
    CHECK(accum[1].id == "call_2");
    CHECK(accum[1].name == "read_file");
}

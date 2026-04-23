#include <catch2/catch_test_macros.hpp>

#include "agent/conversation.h"
#include "agent/system_prompt.h"
#include "tool_registry.h"
#include "tools/tools.h"
#include "agent/agent_core.h"
#include "llm_client.h"

using namespace locus;

// ---- ConversationHistory tests -----------------------------------------------

TEST_CASE("ConversationHistory add and access", "[s0.7]")
{
    ConversationHistory h;
    REQUIRE(h.empty());
    REQUIRE(h.size() == 0);

    h.add({MessageRole::system, "You are helpful."});
    h.add({MessageRole::user, "Hello"});
    h.add({MessageRole::assistant, "Hi there!"});

    REQUIRE(h.size() == 3);
    REQUIRE(h.messages()[0].role == MessageRole::system);
    REQUIRE(h.messages()[1].content == "Hello");
    REQUIRE(h.messages()[2].content == "Hi there!");
}

TEST_CASE("ConversationHistory token estimation", "[s0.7]")
{
    ConversationHistory h;
    h.add({MessageRole::user, "Short message"});
    int tokens = h.estimate_tokens();
    // ~4 chars per token, "Short message" = 13 chars -> ~3 tokens + 4 framing
    REQUIRE(tokens > 0);
    REQUIRE(tokens < 50);
}

TEST_CASE("ConversationHistory JSON round-trip", "[s0.7]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "System prompt"});
    h.add({MessageRole::user, "User question"});

    ChatMessage assistant_msg;
    assistant_msg.role = MessageRole::assistant;
    assistant_msg.content = "Let me check.";
    assistant_msg.tool_calls = {{"call_1", "read_file", R"({"path":"test.txt"})"}};
    h.add(std::move(assistant_msg));

    ChatMessage tool_msg;
    tool_msg.role = MessageRole::tool;
    tool_msg.tool_call_id = "call_1";
    tool_msg.content = "File contents here.";
    h.add(std::move(tool_msg));

    auto j = h.to_json();
    auto h2 = ConversationHistory::from_json(j);

    REQUIRE(h2.size() == 4);
    REQUIRE(h2.messages()[0].role == MessageRole::system);
    REQUIRE(h2.messages()[2].tool_calls.size() == 1);
    REQUIRE(h2.messages()[2].tool_calls[0].name == "read_file");
    REQUIRE(h2.messages()[3].tool_call_id == "call_1");
}

// ---- Compaction tests -------------------------------------------------------

TEST_CASE("Compaction B: drop tool results", "[s0.7]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "System"});
    h.add({MessageRole::user, "Do something"});

    ChatMessage assistant_msg;
    assistant_msg.role = MessageRole::assistant;
    assistant_msg.content = "";
    assistant_msg.tool_calls = {{"call_1", "read_file", "{}"}};
    h.add(std::move(assistant_msg));

    ChatMessage tool_msg;
    tool_msg.role = MessageRole::tool;
    tool_msg.tool_call_id = "call_1";
    tool_msg.content = "This is a very long tool result with lots of data...";
    h.add(std::move(tool_msg));

    h.add({MessageRole::assistant, "Based on the file..."});

    int before = h.estimate_tokens();
    h.drop_tool_results();
    int after = h.estimate_tokens();

    REQUIRE(after <= before);
    REQUIRE(h.messages()[3].content == "[tool result removed for context space]");
    // Non-tool messages should be unchanged.
    REQUIRE(h.messages()[0].content == "System");
    REQUIRE(h.messages()[4].content == "Based on the file...");
}

TEST_CASE("Compaction C: drop oldest turns", "[s0.7]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "System"});
    h.add({MessageRole::user, "First question"});
    h.add({MessageRole::assistant, "First answer"});
    h.add({MessageRole::user, "Second question"});
    h.add({MessageRole::assistant, "Second answer"});
    h.add({MessageRole::user, "Third question"});
    h.add({MessageRole::assistant, "Third answer"});

    REQUIRE(h.size() == 7);

    h.drop_oldest_turns(2);

    // System message preserved. First two turns dropped. Third remains.
    REQUIRE(h.size() == 3);
    REQUIRE(h.messages()[0].role == MessageRole::system);
    REQUIRE(h.messages()[1].content == "Third question");
    REQUIRE(h.messages()[2].content == "Third answer");
}

TEST_CASE("Compaction C: drop turns with tool calls", "[s0.7]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "System"});
    h.add({MessageRole::user, "Read a file"});

    ChatMessage assistant_msg;
    assistant_msg.role = MessageRole::assistant;
    assistant_msg.content = "";
    assistant_msg.tool_calls = {{"call_1", "read_file", "{}"}};
    h.add(std::move(assistant_msg));

    ChatMessage tool_msg;
    tool_msg.role = MessageRole::tool;
    tool_msg.tool_call_id = "call_1";
    tool_msg.content = "File data";
    h.add(std::move(tool_msg));

    h.add({MessageRole::user, "Now explain"});
    h.add({MessageRole::assistant, "Explanation"});

    REQUIRE(h.size() == 6);

    // Drop 1 turn — should remove user + assistant + tool result.
    h.drop_oldest_turns(1);

    REQUIRE(h.size() == 3);  // system + user("Now explain") + assistant("Explanation")
    REQUIRE(h.messages()[0].role == MessageRole::system);
    REQUIRE(h.messages()[1].content == "Now explain");
}

// ---- SystemPromptBuilder tests ----------------------------------------------

TEST_CASE("SystemPromptBuilder assembles all sections", "[s0.7]")
{
    ToolRegistry reg;
    register_builtin_tools(reg);

    WorkspaceMetadata meta;
    meta.root = "d:/test_workspace";
    meta.file_count = 42;
    meta.symbol_count = 100;
    meta.heading_count = 15;

    std::string locus_md = "# My Project\nA test project.";

    std::string prompt = SystemPromptBuilder::build(locus_md, meta, reg);

    // Should contain base instructions.
    REQUIRE(prompt.find("Locus") != std::string::npos);
    // Should contain workspace metadata.
    REQUIRE(prompt.find("42") != std::string::npos);
    REQUIRE(prompt.find("100") != std::string::npos);
    // Should contain LOCUS.md content.
    REQUIRE(prompt.find("My Project") != std::string::npos);
    // Should contain tool names.
    REQUIRE(prompt.find("read_file") != std::string::npos);
    REQUIRE(prompt.find("write_file") != std::string::npos);
    REQUIRE(prompt.find("search_text") != std::string::npos);
}

TEST_CASE("SystemPromptBuilder without LOCUS.md", "[s0.7]")
{
    ToolRegistry reg;
    WorkspaceMetadata meta;
    meta.root = "d:/test";

    std::string prompt = SystemPromptBuilder::build("", meta, reg);

    REQUIRE(prompt.find("LOCUS.md") == std::string::npos);
    REQUIRE(prompt.find("Locus") != std::string::npos);
}

TEST_CASE("LOCUS.md token budget check", "[s0.7]")
{
    // Short content should be fine.
    int short_tokens = SystemPromptBuilder::check_locus_md_budget("Short guide.");
    REQUIRE(short_tokens < 500);

    // Long content should trigger warning (we just check it returns > 500).
    std::string long_md(3000, 'x');  // ~750 tokens
    int long_tokens = SystemPromptBuilder::check_locus_md_budget(long_md);
    REQUIRE(long_tokens > 500);
}

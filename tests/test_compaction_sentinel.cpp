// S6.18 Task B -- progress-preserving sentinel + last-assistant-text pin.
//
// Covers (without spinning up a live LLM, which is the integration suite's
// job): the `is_preserved` heuristic shields the most recent assistant
// free-text message from drop_candidates, and `drop_candidates` itself
// reflects that pin. The hard-trim past oldest_turns path (Task B.2) lives
// in AgentCore::apply_pending_compaction, which depends on a full agent
// thread setup; the unit-level path here verifies the pipeline-side
// preservation behaviour the hard-trim builds on.

#include <catch2/catch_test_macros.hpp>

#include "agent/compaction_pipeline.h"
#include "llm/llm_client.h"
#include "llm/token_counter.h"

#include <string>
#include <vector>

using namespace locus;

namespace {

ChatMessage user_msg(const std::string& body)
{
    ChatMessage m;
    m.role = MessageRole::user;
    m.content = body;
    m.token_estimate = TokenCounter::estimate_message(m);
    return m;
}

ChatMessage assistant_text(const std::string& body)
{
    ChatMessage m;
    m.role = MessageRole::assistant;
    m.content = body;
    m.token_estimate = TokenCounter::estimate_message(m);
    return m;
}

ChatMessage assistant_tool_call(const std::string& id, const std::string& name)
{
    ChatMessage m;
    m.role = MessageRole::assistant;
    ToolCallRequest tc;
    tc.id = id;
    tc.name = name;
    tc.arguments = "{}";
    m.tool_calls.push_back(std::move(tc));
    m.token_estimate = TokenCounter::estimate_message(m);
    return m;
}

ChatMessage tool_result(const std::string& id, const std::string& body)
{
    ChatMessage m;
    m.role = MessageRole::tool;
    m.tool_call_id = id;
    m.content = body;
    m.token_estimate = TokenCounter::estimate_message(m);
    return m;
}

// Long body so the message isn't preserved by the short-message heuristic
// (preserve_short_user_msgs_max_tokens defaults to 500).
std::string long_body(const std::string& tag, std::size_t cap = 4000)
{
    std::string s = "[" + tag + "] ";
    while (s.size() < cap) s += "filler word ";
    return s;
}

} // namespace

TEST_CASE("Last assistant-text message is pinned by is_preserved", "[s6.18][compaction]")
{
    // Shape:
    //   [0] system
    //   [1] user (first user, always pinned)
    //   [2] assistant tool_call
    //   [3] tool_result
    //   [4] assistant text  <-- this is the "last assistant text"
    //   [5] user
    //   [6] assistant tool_call
    //   [7] tool_result
    std::vector<ChatMessage> msgs;
    {
        ChatMessage sys;
        sys.role = MessageRole::system;
        sys.content = "system prompt";
        sys.token_estimate = TokenCounter::estimate_message(sys);
        msgs.push_back(std::move(sys));
    }
    msgs.push_back(user_msg(long_body("first user")));
    msgs.push_back(assistant_tool_call("a1", "search_text"));
    msgs.push_back(tool_result("a1", long_body("tool result a1")));
    msgs.push_back(assistant_text(long_body("the answer I gave")));
    msgs.push_back(user_msg(long_body("second user")));
    msgs.push_back(assistant_tool_call("b1", "read_file"));
    msgs.push_back(tool_result("b1", long_body("tool result b1")));

    CompactionLayerSelection cfg;
    cfg.keep_recent_turns                     = 0;  // disable recent-keep
    cfg.preserve_short_user_msgs_max_tokens   = 0;  // disable short-msg shield
    cfg.preserve_short_tool_calls_max_tokens  = 0;

    // first_user = 1, recent_cut = msgs.size() (keep_recent=0 -> nothing
    // kept by that rule), last_asst_txt = 4.
    REQUIRE(CompactionPipeline::is_preserved(msgs, 4, cfg,
                                             /*first_user=*/1,
                                             /*keep_recent_from=*/msgs.size(),
                                             /*last_asst_txt=*/4));

    // Index 2 (tool_call) is NOT pinned by the new rule.
    REQUIRE_FALSE(CompactionPipeline::is_preserved(msgs, 2, cfg,
                                                   1, msgs.size(), 4));

    // Index 6 (assistant tool_call -- has tool_calls, so NOT a text message)
    // is NOT pinned by the new rule.
    REQUIRE_FALSE(CompactionPipeline::is_preserved(msgs, 6, cfg,
                                                   1, msgs.size(), 4));
}

TEST_CASE("drop_candidates shields turn containing last assistant text",
          "[s6.18][compaction]")
{
    // Build a history where the assistant gave its free-text answer in a
    // turn that's older than keep_recent: the new pin must keep that turn
    // alive even though the heuristic would otherwise nominate it.
    std::vector<ChatMessage> msgs;
    {
        ChatMessage sys;
        sys.role = MessageRole::system;
        sys.content = "system prompt";
        sys.token_estimate = TokenCounter::estimate_message(sys);
        msgs.push_back(std::move(sys));
    }
    msgs.push_back(user_msg(long_body("user-1 seed")));
    // Turn 2 -- this is the LAST assistant-text turn we want to keep.
    msgs.push_back(user_msg(long_body("user-2 question")));
    msgs.push_back(assistant_text(long_body("free-text answer from turn 2")));
    // Three follow-up turns that are pure tool work (no assistant text).
    msgs.push_back(user_msg(long_body("user-3 follow-up")));
    msgs.push_back(assistant_tool_call("c1", "read_file"));
    msgs.push_back(tool_result("c1", long_body("c1 body")));
    msgs.push_back(user_msg(long_body("user-4 follow-up")));
    msgs.push_back(assistant_tool_call("d1", "search_text"));
    msgs.push_back(tool_result("d1", long_body("d1 body")));
    msgs.push_back(user_msg(long_body("user-5 follow-up")));
    msgs.push_back(assistant_tool_call("e1", "edit_file"));
    msgs.push_back(tool_result("e1", long_body("e1 body")));

    CompactionLayerSelection cfg;
    cfg.keep_recent_turns                     = 1;   // keep just user-5's group
    cfg.preserve_short_user_msgs_max_tokens   = 0;
    cfg.preserve_short_tool_calls_max_tokens  = 0;

    auto cands = CompactionPipeline::drop_candidates(msgs, cfg);

    // Walk the returned spans and assert that none of them contains the
    // assistant-text message at index 3.
    bool turn2_spared = true;
    for (const auto& sp : cands) {
        if (sp.begin <= 3 && 3 < sp.end) {
            turn2_spared = false;
            break;
        }
    }
    REQUIRE(turn2_spared);
}

TEST_CASE("PipelineResult.llm_summary_text empty when Layer 6 didn't run",
          "[s6.18][compaction]")
{
    // No llm pointer -> Layer 6 short-circuits. Verify the field stays
    // empty so AgentCore falls back to the legacy footnote shape.
    ConversationHistory hist;
    {
        ChatMessage sys;
        sys.role = MessageRole::system;
        sys.content = "system";
        sys.token_estimate = TokenCounter::estimate_message(sys);
        hist.add(std::move(sys));
    }
    hist.add(user_msg("hi"));
    hist.add(assistant_text("hello"));

    CompactionLayerSelection cfg;
    LLMConfig dummy_cfg;
    auto result = CompactionPipeline::run(hist, cfg,
                                          /*target_tokens=*/100000,
                                          /*llm=*/nullptr, dummy_cfg);
    REQUIRE(result.llm_summary_text.empty());
}

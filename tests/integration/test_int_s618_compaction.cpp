// S6.18 Task B acceptance tests.
//
// Task B.1: When the cascade can't reach target via the deterministic
//           layers (1/2/3/5), Layer 6 must fire AND the AgentCore-side
//           footnote must read "Progress so far: <summary>". The
//           progress-preserving sentinel is what makes long sessions
//           recoverable after compaction. We also verify the
//           last-assistant-text pin survives.
//
// Task B.2: When `drop_oldest_turns` is enabled but the cascade still
//           reports `reached=no` (a "zero-stripping shape" history where
//           the preservation heuristic shields every candidate), AgentCore
//           must hard-trim oldest turns until target is hit. The
//           pipeline-side path here only verifies the structural
//           preservation logic (so we know Layer 5 alone wouldn't have
//           reached); the hard-trim itself lives in AgentCore and is
//           exercised by the [s6.18][compaction] unit tests + manual
//           agentic runs.
//
// Manual-only -- tagged `[integration][llm][s6.18][compaction]`.

#include "harness_fixture.h"

#include "agent/compaction_pipeline.h"
#include "agent/conversation.h"
#include "llm/llm_client.h"
#include "llm/token_counter.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace locus;
using namespace locus::integration;
namespace fs = std::filesystem;

namespace {

fs::path archive_dir()
{
    return fs::path(LOCUS_INT_TEST_WORKSPACE_DIR) /
           "tests" / "ui_automation" / "output" / "agentic_TestLocalVibe2";
}

ConversationHistory load_archive(int counter)
{
    fs::path p = archive_dir() / ("history.before-compact-" +
                                  std::to_string(counter) + ".json");
    REQUIRE(fs::exists(p));

    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    auto j = nlohmann::json::parse(ss.str());

    ConversationHistory hist;
    for (const auto& m : j["messages"])
        hist.add(ChatMessage::from_json(m));
    return hist;
}

constexpr int k_pass5_target_tokens = 9831;

// Find the index of the most recent assistant-text message (mirror of the
// helper in compaction_pipeline.cpp). Returns -1 if none.
int last_assistant_text_idx(const std::vector<ChatMessage>& msgs)
{
    for (int i = static_cast<int>(msgs.size()) - 1; i >= 0; --i) {
        const auto& m = msgs[i];
        if (m.role != MessageRole::assistant) continue;
        if (!m.tool_calls.empty()) continue;
        if (m.content.empty()) continue;
        return i;
    }
    return -1;
}

} // namespace

TEST_CASE("S6.18 B.1 -- Layer 6 produces a non-empty llm_summary_text "
          "when forced to run",
          "[integration][llm][s6.18][compaction]")
{
    // Construct a synthetic history where Layers 1/2/3 cannot reach target
    // (no redundant tool calls, no large tool results, no <think> blocks)
    // and drop_oldest_turns is disabled. The only remaining lever is
    // Layer 6 -- verify that S6.18's "fire unconditionally on reached=no"
    // gate AND the PipelineResult::llm_summary_text propagation both work.
    auto& h = harness();

    ConversationHistory hist;
    {
        ChatMessage sys;
        sys.role = MessageRole::system;
        sys.content = "You are a helpful assistant.";
        sys.token_estimate = TokenCounter::estimate_message(sys);
        hist.add(std::move(sys));
    }

    // 10 turns of long user/assistant text. Each side is big enough to
    // bypass the short-msg preservation (over 500 tokens / message) so
    // Layer 6's drop_candidates will see them as candidates. Each
    // assistant message has content but no tool_calls -- that's important
    // for the "pin last assistant text" rule below.
    auto big = [](char c) {
        return std::string(3200, c);  // ~800 tokens at 4 chars/token
    };
    for (int i = 0; i < 10; ++i) {
        ChatMessage u;
        u.role = MessageRole::user;
        u.content = "User turn " + std::to_string(i) + ": " + big('u');
        u.token_estimate = TokenCounter::estimate_message(u);
        hist.add(std::move(u));

        ChatMessage a;
        a.role = MessageRole::assistant;
        a.content = "Assistant turn " + std::to_string(i) + ": " + big('a');
        a.token_estimate = TokenCounter::estimate_message(a);
        hist.add(std::move(a));
    }

    int before = hist.estimate_tokens();
    INFO("synthetic history: " << before << " tokens, "
         << hist.size() << " messages");

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = true;
    sel.strip_large_tool_bodies     = true;
    sel.drop_old_reasoning          = true;
    sel.drop_oldest_turns           = false;          // force Layer 6
    sel.llm_summary                 = true;
    sel.keep_recent_turns           = 1;              // small recent window
    sel.preserve_short_user_msgs_max_tokens  = 0;    // disable short-msg shield
    sel.preserve_short_tool_calls_max_tokens = 0;

    int target = before / 2;
    auto result = CompactionPipeline::run(hist, sel, target, &h.llm(), {});

    INFO("pipeline result: " << result.after_tokens
         << " (target " << target
         << ", reached=" << (result.reached_target ? "yes" : "no") << ")");

    // Find Layer 6 in the result
    const LayerResult* layer6 = nullptr;
    for (const auto& lr : result.layers) {
        if (lr.name.find("LLM summary") != std::string::npos) {
            layer6 = &lr;
            break;
        }
    }
    REQUIRE(layer6 != nullptr);
    REQUIRE(layer6->ran);

    // S6.18 B.1 -- the summary must propagate to the PipelineResult so
    // AgentCore can use it in the footnote.
    INFO("llm_summary_text head: " << result.llm_summary_text.substr(0, 200));
    REQUIRE_FALSE(result.llm_summary_text.empty());
    REQUIRE(result.llm_summary_text.size() > 30);  // non-trivial prose
}

TEST_CASE("S6.18 B.1 -- last-assistant-text message survives compaction",
          "[integration][llm][s6.18][compaction]")
{
    // Build a synthetic history where the most recent assistant message is
    // free-text (the "pin candidate"), with several older drop-eligible
    // turns. After compaction the pinned message must still appear in the
    // post-compaction history verbatim.
    auto& h = harness();

    ConversationHistory hist;
    {
        ChatMessage sys;
        sys.role = MessageRole::system;
        sys.content = "You are a helpful assistant.";
        sys.token_estimate = TokenCounter::estimate_message(sys);
        hist.add(std::move(sys));
    }

    auto big = [](char c) { return std::string(3200, c); };
    // 8 older turns
    for (int i = 0; i < 8; ++i) {
        ChatMessage u;
        u.role = MessageRole::user;
        u.content = "User turn " + std::to_string(i) + ": " + big('u');
        u.token_estimate = TokenCounter::estimate_message(u);
        hist.add(std::move(u));

        ChatMessage a;
        a.role = MessageRole::assistant;
        a.content = "Assistant turn " + std::to_string(i) + ": " + big('a');
        a.token_estimate = TokenCounter::estimate_message(a);
        hist.add(std::move(a));
    }

    // A clearly-identifiable last assistant-text message we expect to
    // survive.
    const std::string pin_marker =
        "SENTINEL-PIN-MARKER-DO-NOT-DROP-2026-05-28";

    ChatMessage final_user;
    final_user.role = MessageRole::user;
    final_user.content = "summarize what we did";
    final_user.token_estimate = TokenCounter::estimate_message(final_user);
    hist.add(std::move(final_user));

    ChatMessage final_asst;
    final_asst.role = MessageRole::assistant;
    final_asst.content = "Here is the recap: " + pin_marker
                       + ". We covered turns 0-7 of the discussion.";
    final_asst.token_estimate = TokenCounter::estimate_message(final_asst);
    hist.add(std::move(final_asst));

    int before = hist.estimate_tokens();
    INFO("synthetic pin history: " << before << " tokens, "
         << hist.size() << " messages");

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = true;
    sel.strip_large_tool_bodies     = true;
    sel.drop_old_reasoning          = true;
    sel.drop_oldest_turns           = true;           // exercise Layer 5
    sel.llm_summary                 = true;
    sel.keep_recent_turns           = 0;              // disable recent-keep
    sel.preserve_short_user_msgs_max_tokens  = 0;
    sel.preserve_short_tool_calls_max_tokens = 0;

    int target = before / 4;  // require deep compaction
    auto result = CompactionPipeline::run(hist, sel, target, &h.llm(), {});

    INFO("after compaction: " << result.after_tokens << " tokens");

    // Walk the post-compaction history -- the pin marker must still appear
    // somewhere on an assistant message with no tool_calls.
    bool found = false;
    int pin_msg_count = 0;
    for (const auto& m : hist.messages()) {
        if (m.role != MessageRole::assistant) continue;
        if (!m.tool_calls.empty()) continue;
        if (m.content.find(pin_marker) != std::string::npos) {
            found = true;
            ++pin_msg_count;
        }
    }
    INFO("pin marker found on " << pin_msg_count << " message(s) after compaction");
    REQUIRE(found);
}

TEST_CASE("S6.18 B.2 -- zero-stripping synthetic shape needs hard-trim",
          "[integration][s6.18][compaction]")
{
    // Construct a history where every turn is "essential" by the
    // preservation heuristic (short user msgs, short assistant texts) so
    // drop_candidates returns nothing. Then run the pipeline with
    // drop_oldest_turns=true and verify it canNOT reach target by itself --
    // hard-trim (AgentCore-side) is the only remaining lever, and the
    // pipeline correctly leaves the work for that path.
    //
    // The AgentCore-side hard-trim itself runs on the agent thread and is
    // verified by the [s6.18][compaction] unit tests + by manual agentic
    // run inspection (warn `CompactionPipeline: saturated, hard-trimmed N
    // turn(s)` in .locus/locus.log).
    ConversationHistory hist;

    {
        ChatMessage sys;
        sys.role = MessageRole::system;
        sys.content = std::string(2000, 's');  // bulky system to push token count
        sys.token_estimate = TokenCounter::estimate_message(sys);
        hist.add(std::move(sys));
    }

    // Add a bunch of short turns (each survives preservation because they
    // fit under the short-message thresholds).
    for (int i = 0; i < 20; ++i) {
        ChatMessage u;
        u.role = MessageRole::user;
        u.content = "Short question #" + std::to_string(i);
        u.token_estimate = TokenCounter::estimate_message(u);
        hist.add(std::move(u));

        ChatMessage a;
        a.role = MessageRole::assistant;
        a.content = "Short answer #" + std::to_string(i);
        a.token_estimate = TokenCounter::estimate_message(a);
        hist.add(std::move(a));
    }

    int before = hist.estimate_tokens();
    INFO("synthetic 'every turn essential' history: " << before << " tokens, "
         << hist.size() << " messages");

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = true;
    sel.strip_large_tool_bodies     = true;
    sel.drop_old_reasoning          = true;
    sel.drop_oldest_turns           = true;
    sel.llm_summary                 = true;
    sel.preserve_short_user_msgs_max_tokens  = 500;
    sel.preserve_short_tool_calls_max_tokens = 500;

    int target = before / 2;  // halve

    // No LLM -- prove the deterministic layers (1/2/3/5) by themselves can't
    // beat a fully-preserved history. AgentCore's hard-trim is what closes
    // the gap; that path's unit tests are in test_compaction_sentinel.cpp.
    auto result = CompactionPipeline::run(hist, sel, target, nullptr, {});

    INFO("pipeline result: " << result.after_tokens
         << " (target " << target
         << ", reached=" << (result.reached_target ? "yes" : "no") << ")");

    // Structural assertion: the deterministic cascade with everything
    // preserved cannot reach target. drop_candidates returned empty (every
    // turn shielded by short-msg heuristic), so Layer 5 had nothing to do.
    // This is the precondition AgentCore looks at when deciding to hard-trim.
    REQUIRE_FALSE(result.reached_target);
    REQUIRE(result.after_tokens > target);
}

TEST_CASE("S6.18 B.2 -- live agent run_compaction fires hard-trim warn",
          "[integration][llm][s6.18][compaction]")
{
    // Drive the AgentCore-side hard-trim path end-to-end. Construct a
    // synthetic session on disk where every turn is short enough to be
    // shielded by the short-msg heuristic, restore it via load_session,
    // then invoke run_compaction with drop_oldest_turns=true. The pipeline
    // will return reached=false; AgentCore's hard-trim then loops until
    // tokens are under target, emitting `CompactionPipeline: saturated,
    // hard-trimmed N turn(s)` to the integration_test.log.
    //
    // Verifies the live-path B.2 acceptance via log inspection.
    auto& h = harness();
    auto& sessions = h.agent().sessions();

    fs::path sess_dir = sessions.sessions_dir();
    fs::create_directories(sess_dir);
    std::string session_id = "test_s618_hardtrim";
    fs::path session_file = sess_dir / (session_id + ".json");

    nlohmann::json j;
    j["id"] = session_id;
    j["created_at"] = 1779000000;
    j["last_opened_at"] = 1779000000;
    nlohmann::json msgs = nlohmann::json::array();
    {
        nlohmann::json m;
        m["role"] = "system";
        m["content"] = std::string(2000, 's');
        msgs.push_back(m);
    }
    for (int i = 0; i < 20; ++i) {
        nlohmann::json mu, ma;
        mu["role"] = "user";
        mu["content"] = "Short question #" + std::to_string(i);
        msgs.push_back(mu);
        ma["role"] = "assistant";
        ma["content"] = "Short answer #" + std::to_string(i);
        msgs.push_back(ma);
    }
    j["messages"] = msgs;
    j["message_count"] = static_cast<int>(msgs.size());
    j["estimated_tokens"] = 5000;
    {
        std::ofstream out(session_file);
        out << j.dump(2);
    }

    // Snapshot integration_test.log size so we can read only what's
    // appended during this test.
    fs::path log_path = h.workspace().root() / ".locus" / "integration_test.log";
    std::uintmax_t log_baseline = 0;
    if (fs::exists(log_path)) {
        std::error_code ec;
        log_baseline = fs::file_size(log_path, ec);
    }

    // Load the synthetic session into the agent's history. load_session
    // runs on the agent thread; poll until the in-memory history reflects
    // it.
    h.agent().load_session(session_id);
    for (int i = 0; i < 100 && h.agent().history().size() < msgs.size(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(h.agent().history().size() == msgs.size());

    int before = h.agent().history().estimate_tokens();
    INFO("loaded synthetic history: " << before << " tokens, "
         << h.agent().history().size() << " messages");

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = true;
    sel.strip_large_tool_bodies     = true;
    sel.drop_old_reasoning          = true;
    sel.drop_oldest_turns           = true;
    sel.llm_summary                 = true;
    sel.preserve_short_user_msgs_max_tokens  = 500;
    sel.preserve_short_tool_calls_max_tokens = 500;

    int target = before / 3;  // require deep compaction past preservation
    h.agent().run_compaction(sel, target, "");

    // Wait for compaction to land (history shrinks below `before`).
    bool reached = false;
    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (h.agent().history().estimate_tokens() < before) {
            reached = true;
            break;
        }
    }
    REQUIRE(reached);

    int after = h.agent().history().estimate_tokens();
    INFO("after compaction: " << after << " tokens (target " << target << ")");
    // Hard-trim is bounded by the floor: system[0] + first user + at least
    // one recent user-assistant pair survive (users.size() < 3 break
    // condition). With a bulky 2000-char system message that floor sits
    // around 500-600 tokens; targets below the floor are unreachable. The
    // contract is "hard-trim shrank the history substantially", not
    // "history is below target".
    REQUIRE(after < before);
    REQUIRE(after < (before * 3) / 4);  // dropped at least 25%

    // Force log flush (spdlog flush_on(warn) means the warn already flushed)
    // then read the appended tail.
    spdlog::default_logger()->flush();
    std::string log_tail;
    if (fs::exists(log_path)) {
        std::ifstream lf(log_path, std::ios::binary);
        lf.seekg(static_cast<std::streamoff>(log_baseline));
        std::ostringstream ss;
        ss << lf.rdbuf();
        log_tail = ss.str();
    }
    INFO("log tail size: " << log_tail.size() << " bytes");
    // Spot the warn line. Either spelling ("hard-trimmed" or "saturated")
    // is acceptable since both appear in the same warn message body.
    bool saw_hardtrim = log_tail.find("hard-trimmed") != std::string::npos
                     || log_tail.find("saturated") != std::string::npos;
    if (!saw_hardtrim) {
        INFO("log tail (last 1500 chars): "
             << log_tail.substr(log_tail.size() > 1500
                                ? log_tail.size() - 1500 : 0));
    }
    REQUIRE(saw_hardtrim);

    std::error_code ec;
    fs::remove(session_file, ec);
}

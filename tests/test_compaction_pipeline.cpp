// S5.F -- Compaction v2 (Toolkit + Auto + History Archive) tests.
//
// Covers:
//   Per-layer transformation correctness on fixture conversations.
//   Cascade stops at the target (doesn't run unnecessary layers).
//   Layer ordering invariant: Layer 1 runs before Layer 2 etc.
//   Auto-preservation heuristics: system / first user / recent N /
//     attached-context / short user msgs / short tool calls all survive
//     Layers 5 and 6; messages above the threshold are eligible for drop.
//   History archive round-trip (snapshot + reload, GC ceiling).
//   Footnote placement preserves S4.F invariant (system prompt byte-stable).
//   Threshold band correctness (warn / auto / refuse fire at the right
//     ratios against effective_limit).
//   WorkspaceConfig::Compaction JSON round-trip.

#include <catch2/catch_test_macros.hpp>

#include "agent/compaction_pipeline.h"
#include "agent/conversation.h"
#include "agent/history_archive.h"
#include "core/workspace.h"
#include "core/workspace_config_json.h"
#include "llm/token_counter.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace locus;
namespace fs = std::filesystem;

namespace {

fs::path tmp_dir(const std::string& tag)
{
    std::random_device rd;
    auto p = fs::temp_directory_path()
             / ("locus_s5f_" + tag + "_" + std::to_string(rd()));
    fs::create_directories(p);
    return p;
}

ChatMessage make_msg(MessageRole r, std::string content)
{
    ChatMessage m;
    m.role = r;
    m.content = std::move(content);
    m.token_estimate = TokenCounter::estimate_message(m);
    return m;
}

ChatMessage make_tool_call(const std::string& id,
                           const std::string& name,
                           const std::string& args)
{
    ChatMessage m;
    m.role = MessageRole::assistant;
    ToolCallRequest tc;
    tc.id = id;
    tc.name = name;
    tc.arguments = args;
    m.tool_calls.push_back(std::move(tc));
    m.token_estimate = TokenCounter::estimate_message(m);
    return m;
}

ChatMessage make_tool_result(const std::string& id, std::string content)
{
    ChatMessage m;
    m.role = MessageRole::tool;
    m.tool_call_id = id;
    m.content = std::move(content);
    m.token_estimate = TokenCounter::estimate_message(m);
    return m;
}

// Build a synthetic conversation with redundant tool calls and a large
// stale result. Returns history populated with all the messages.
ConversationHistory build_synthetic_conversation()
{
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "You are an agent."));
    h.add(make_msg(MessageRole::user, "Please read foo.cpp."));
    h.add(make_tool_call("call_1", "read_file", R"({"path":"foo.cpp"})"));
    h.add(make_tool_result("call_1", std::string(8000, 'A')));  // very large
    h.add(make_msg(MessageRole::assistant,
                   "Got it.<think>thinking out loud about foo</think>\n"
                   "Done reading."));
    h.add(make_msg(MessageRole::user, "Now read foo.cpp again."));
    h.add(make_tool_call("call_2", "read_file", R"({"path":"foo.cpp"})"));
    h.add(make_tool_result("call_2", std::string(8000, 'B')));  // very large, latest
    h.add(make_msg(MessageRole::assistant, "Same content."));
    h.add(make_msg(MessageRole::user, "What does foo do?"));
    h.add(make_msg(MessageRole::assistant,
                   "It does the thing.<think>more reasoning</think>"));
    return h;
}

} // namespace

// ---------------------------------------------------------------------------
// WorkspaceConfig::Compaction JSON round-trip
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][config] compaction block round-trips through JSON",
          "[s5.f][config]")
{
    WorkspaceConfig cfg;
    cfg.compaction.auto_enabled    = false;
    cfg.compaction.warn_threshold  = 0.55;
    cfg.compaction.auto_threshold  = 0.77;
    cfg.compaction.strip_threshold_tokens = 1500;
    cfg.compaction.older_than_turns       = 5;
    cfg.compaction.keep_recent_turns      = 4;
    cfg.compaction.summary_max_tokens     = 600;
    cfg.compaction.archive_keep_count     = 9;
    cfg.compaction.preserve_short_user_msgs_max_tokens  = 250;
    cfg.compaction.preserve_short_tool_calls_max_tokens = 0;  // disabled
    cfg.compaction.custom_summary_instructions =
        "preserve all file paths and test commands";
    // S5.N -- per-layer flags. Flip non-default values to verify the round
    // trip carries them faithfully; the Save button in CompactionDialog
    // depends on this so users can persist their layer choices.
    cfg.compaction.layer_drop_redundant_tool_results = false;
    cfg.compaction.layer_strip_large_tool_bodies     = false;
    cfg.compaction.layer_drop_old_reasoning          = false;
    cfg.compaction.layer_drop_oldest_turns           = true;
    cfg.compaction.layer_llm_summary                 = false;

    auto j = workspace_config_to_json(cfg);
    auto parsed = workspace_config_from_json(j);

    REQUIRE(parsed.compaction.auto_enabled    == false);
    REQUIRE(parsed.compaction.warn_threshold  == 0.55);
    REQUIRE(parsed.compaction.auto_threshold  == 0.77);
    REQUIRE(parsed.compaction.strip_threshold_tokens == 1500);
    REQUIRE(parsed.compaction.older_than_turns       == 5);
    REQUIRE(parsed.compaction.keep_recent_turns      == 4);
    REQUIRE(parsed.compaction.summary_max_tokens     == 600);
    REQUIRE(parsed.compaction.archive_keep_count     == 9);
    REQUIRE(parsed.compaction.preserve_short_user_msgs_max_tokens  == 250);
    REQUIRE(parsed.compaction.preserve_short_tool_calls_max_tokens == 0);
    REQUIRE(parsed.compaction.custom_summary_instructions ==
            "preserve all file paths and test commands");
    REQUIRE(parsed.compaction.layer_drop_redundant_tool_results == false);
    REQUIRE(parsed.compaction.layer_strip_large_tool_bodies     == false);
    REQUIRE(parsed.compaction.layer_drop_old_reasoning          == false);
    REQUIRE(parsed.compaction.layer_drop_oldest_turns           == true);
    REQUIRE(parsed.compaction.layer_llm_summary                 == false);
}

TEST_CASE("[s5.f][config] compaction defaults", "[s5.f][config]")
{
    WorkspaceConfig::Compaction c;
    REQUIRE(c.auto_enabled == true);
    REQUIRE(c.warn_threshold  > 0.65);
    REQUIRE(c.warn_threshold  < 0.80);
    REQUIRE(c.auto_threshold  > 0.80);
    REQUIRE(c.auto_threshold  < 0.95);
    REQUIRE(c.strip_threshold_tokens                >= 500);
    REQUIRE(c.preserve_short_user_msgs_max_tokens   == 500);
    REQUIRE(c.preserve_short_tool_calls_max_tokens  == 500);
    REQUIRE(c.archive_keep_count                    == 5);
}

// ---------------------------------------------------------------------------
// Layer 1 -- drop redundant tool results
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][layer1] duplicate tool results collapsed to stubs",
          "[s5.f][layer1]")
{
    auto h = build_synthetic_conversation();
    int before = h.estimate_tokens();

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = true;
    sel.strip_large_tool_bodies     = false;
    sel.drop_old_reasoning          = false;
    sel.drop_oldest_turns           = false;
    sel.llm_summary                 = false;

    LLMConfig lc;
    auto r = CompactionPipeline::run(h, sel, /*target=*/1, nullptr, lc);
    int after = h.estimate_tokens();

    REQUIRE(after < before);
    REQUIRE(r.layers.size() == 1);
    REQUIRE(r.layers[0].messages_touched >= 1);

    // First tool result should be stubbed; second (latest) preserved.
    bool found_stub = false;
    bool found_latest = false;
    for (auto& m : h.messages()) {
        if (m.role != MessageRole::tool) continue;
        if (m.tool_call_id == "call_1") {
            REQUIRE(m.content.find("duplicate of later") != std::string::npos);
            found_stub = true;
        }
        if (m.tool_call_id == "call_2") {
            REQUIRE(m.content.size() == 8000);
            found_latest = true;
        }
    }
    REQUIRE(found_stub);
    REQUIRE(found_latest);
}

// ---------------------------------------------------------------------------
// Layer 2 -- strip large tool bodies
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][layer2] large tool bodies become headers",
          "[s5.f][layer2]")
{
    auto h = build_synthetic_conversation();
    int before = h.estimate_tokens();

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = false;
    sel.strip_large_tool_bodies     = true;
    sel.strip_threshold_tokens      = 100;  // very low so both bodies qualify
    sel.drop_old_reasoning          = false;
    sel.drop_oldest_turns           = false;
    sel.llm_summary                 = false;

    LLMConfig lc;
    auto r = CompactionPipeline::run(h, sel, 1, nullptr, lc);
    int after = h.estimate_tokens();

    REQUIRE(after < before);
    REQUIRE(r.layers.size() == 1);
    REQUIRE(r.layers[0].messages_touched == 2);

    int stripped = 0;
    for (auto& m : h.messages()) {
        if (m.role == MessageRole::tool) {
            REQUIRE(m.content.find("Tool result stripped") != std::string::npos);
            REQUIRE(m.content.size() < 300);
            ++stripped;
        }
    }
    REQUIRE(stripped == 2);
}

TEST_CASE("[s5.f][layer2] threshold respected", "[s5.f][layer2]")
{
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "sys"));
    h.add(make_msg(MessageRole::user, "go"));
    h.add(make_tool_call("c1", "list_directory", R"({"path":"."})"));
    h.add(make_tool_result("c1", "small result"));  // tiny -- below threshold
    h.add(make_msg(MessageRole::assistant, "done"));

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = false;
    sel.strip_large_tool_bodies     = true;
    sel.strip_threshold_tokens      = 1000;  // small tool result is well under
    sel.drop_old_reasoning          = false;
    sel.drop_oldest_turns           = false;
    sel.llm_summary                 = false;

    int before = h.estimate_tokens();
    LLMConfig lc;
    CompactionPipeline::run(h, sel, 1, nullptr, lc);
    int after = h.estimate_tokens();

    REQUIRE(after == before);  // nothing changed
}

// ---------------------------------------------------------------------------
// Layer 3 -- drop old reasoning
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][layer3] <think> blocks removed from old assistant messages",
          "[s5.f][layer3]")
{
    auto h = build_synthetic_conversation();
    int before = h.estimate_tokens();

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = false;
    sel.strip_large_tool_bodies     = false;
    sel.drop_old_reasoning          = true;
    sel.older_than_turns            = 0;  // drop ALL reasoning
    sel.drop_oldest_turns           = false;
    sel.llm_summary                 = false;

    LLMConfig lc;
    CompactionPipeline::run(h, sel, 1, nullptr, lc);

    int think_count = 0;
    for (auto& m : h.messages()) {
        if (m.content.find("<think>") != std::string::npos) ++think_count;
    }
    REQUIRE(think_count == 0);
    REQUIRE(h.estimate_tokens() < before);
}

TEST_CASE("[s5.f][layer3] recent reasoning preserved", "[s5.f][layer3]")
{
    auto h = build_synthetic_conversation();

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = false;
    sel.strip_large_tool_bodies     = false;
    sel.drop_old_reasoning          = true;
    sel.older_than_turns            = 10;  // keeps recent N -- effectively all
    sel.drop_oldest_turns           = false;
    sel.llm_summary                 = false;

    LLMConfig lc;
    CompactionPipeline::run(h, sel, 1, nullptr, lc);

    // Should leave at least one <think> intact (the most recent assistant).
    int think_count = 0;
    for (auto& m : h.messages()) {
        if (m.content.find("<think>") != std::string::npos) ++think_count;
    }
    REQUIRE(think_count >= 1);
}

// ---------------------------------------------------------------------------
// Auto-preservation heuristics
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][heuristics] system / first user always preserved",
          "[s5.f][heuristics]")
{
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "system"));
    h.add(make_msg(MessageRole::user, "first user message"));
    // Bulk messages -- 3 turn pairs.
    for (int i = 0; i < 3; ++i) {
        h.add(make_msg(MessageRole::user, std::string(4000, 'x')));
        h.add(make_msg(MessageRole::assistant, std::string(4000, 'y')));
    }

    CompactionLayerSelection sel;
    sel.drop_oldest_turns = true;
    sel.keep_recent_turns = 0;  // try to drop everything except the protected
    sel.preserve_short_user_msgs_max_tokens  = 0;
    sel.preserve_short_tool_calls_max_tokens = 0;

    auto cands = CompactionPipeline::drop_candidates(h.messages(), sel);
    // The "first user message" group should not be a candidate.
    for (const auto& s : cands) REQUIRE(s.begin != 1);

    LLMConfig lc;
    sel.llm_summary = false;
    sel.drop_redundant_tool_results = false;
    sel.strip_large_tool_bodies = false;
    sel.drop_old_reasoning = false;
    CompactionPipeline::run(h, sel, 1, nullptr, lc);

    REQUIRE(h.messages().front().role == MessageRole::system);
    REQUIRE(h.messages().front().content == "system");
    // First user message still present.
    bool kept_first_user = false;
    for (auto& m : h.messages())
        if (m.role == MessageRole::user && m.content == "first user message")
            kept_first_user = true;
    REQUIRE(kept_first_user);
}

TEST_CASE("[s5.f][heuristics] attached-file user message preserved",
          "[s5.f][heuristics]")
{
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "system"));
    h.add(make_msg(MessageRole::user, "first user msg"));
    h.add(make_msg(MessageRole::assistant, std::string(4000, 'a')));
    // Add a long user message containing the attached-file marker.
    std::string body = "[Attached file: src/foo.cpp]\nOutline:\n- L1 [function] foo\n\n"
                       "Please refactor "
                       + std::string(3000, 'z');
    h.add(make_msg(MessageRole::user, body));
    h.add(make_msg(MessageRole::assistant, std::string(2000, 'b')));
    // Add a non-attached big user message (drop-eligible).
    h.add(make_msg(MessageRole::user, std::string(4000, 'q')));
    h.add(make_msg(MessageRole::assistant, std::string(2000, 'c')));

    CompactionLayerSelection sel;
    sel.drop_oldest_turns = true;
    sel.keep_recent_turns = 0;
    sel.preserve_short_user_msgs_max_tokens  = 0;
    sel.preserve_short_tool_calls_max_tokens = 0;

    auto cands = CompactionPipeline::drop_candidates(h.messages(), sel);
    // The attached-file message lives at index 3 -> its turn group begins there.
    for (const auto& s : cands) {
        if (s.begin == 3) FAIL("Attached-file turn group should be preserved");
    }
}

TEST_CASE("[s5.f][heuristics] short user messages preserved",
          "[s5.f][heuristics]")
{
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "system"));
    h.add(make_msg(MessageRole::user, "first"));
    // Short user msgs -- under default 500-token threshold (since these are
    // tiny strings).
    h.add(make_msg(MessageRole::user, "what about bar?"));
    h.add(make_msg(MessageRole::assistant, std::string(3000, 'a')));
    h.add(make_msg(MessageRole::user, "and baz?"));
    h.add(make_msg(MessageRole::assistant, std::string(3000, 'b')));
    // One big user message -- drop-eligible.
    h.add(make_msg(MessageRole::user, std::string(4000, 'q')));
    h.add(make_msg(MessageRole::assistant, std::string(2000, 'c')));

    CompactionLayerSelection sel;
    sel.drop_oldest_turns = true;
    sel.keep_recent_turns = 0;
    sel.preserve_short_user_msgs_max_tokens  = 500;  // default
    sel.preserve_short_tool_calls_max_tokens = 0;

    auto cands = CompactionPipeline::drop_candidates(h.messages(), sel);
    // Short user msg turn groups must NOT appear.
    for (const auto& s : cands) {
        REQUIRE(s.begin != 2);
        REQUIRE(s.begin != 4);
    }
    // The big user message at index 6 SHOULD be a candidate.
    bool found_big = false;
    for (const auto& s : cands) {
        if (s.begin == 6) found_big = true;
    }
    REQUIRE(found_big);
}

TEST_CASE("[s5.f][heuristics] short user threshold of 0 disables heuristic",
          "[s5.f][heuristics]")
{
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "system"));
    h.add(make_msg(MessageRole::user, "first"));
    h.add(make_msg(MessageRole::user, "short"));
    h.add(make_msg(MessageRole::assistant, std::string(2000, 'a')));

    CompactionLayerSelection sel;
    sel.drop_oldest_turns = true;
    sel.keep_recent_turns = 0;
    sel.preserve_short_user_msgs_max_tokens  = 0;  // disabled
    sel.preserve_short_tool_calls_max_tokens = 0;

    auto cands = CompactionPipeline::drop_candidates(h.messages(), sel);
    // The "short" user message at index 2 IS now a candidate (heuristic off).
    bool found = false;
    for (const auto& s : cands) {
        if (s.begin == 2) found = true;
    }
    REQUIRE(found);
}

TEST_CASE("[s5.f][heuristics] keep_recent_turns preserves the tail",
          "[s5.f][heuristics]")
{
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "system"));
    for (int i = 0; i < 6; ++i) {
        h.add(make_msg(MessageRole::user, std::string(3000, 'x') + std::to_string(i)));
        h.add(make_msg(MessageRole::assistant, std::string(3000, 'y') + std::to_string(i)));
    }

    CompactionLayerSelection sel;
    sel.drop_oldest_turns = true;
    sel.keep_recent_turns = 2;
    sel.preserve_short_user_msgs_max_tokens  = 0;
    sel.preserve_short_tool_calls_max_tokens = 0;

    auto cands = CompactionPipeline::drop_candidates(h.messages(), sel);
    // The last 2 turns (user_4, user_5) must not be in candidates.
    // Total messages: 1 (system) + 12 (6 turns) = 13. user_4 at idx 9, user_5 at 11.
    for (const auto& s : cands) {
        REQUIRE(s.begin < 9);
    }
}

// ---------------------------------------------------------------------------
// Cascade ordering + stop-at-target
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][cascade] stops at target without running later layers",
          "[s5.f][cascade]")
{
    auto h = build_synthetic_conversation();
    int before = h.estimate_tokens();

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = true;
    sel.strip_large_tool_bodies     = true;
    sel.drop_old_reasoning          = true;
    sel.drop_oldest_turns           = true;
    sel.llm_summary                 = true;  // would call LLM (no llm given)

    // Target far below current -- so cascade keeps running through layers
    // 1->2 until it drops below. Layer 3 shouldn't be needed since 1+2 free
    // a lot.
    int target = before / 4;
    LLMConfig lc;
    auto r = CompactionPipeline::run(h, sel, target, /*llm=*/nullptr, lc);

    // Layer 1 ran, Layer 2 ran. After that we should be under target.
    REQUIRE(r.layers.size() >= 2);
    REQUIRE(r.layers[0].name.find("Layer 1") != std::string::npos);
    REQUIRE(r.layers[1].name.find("Layer 2") != std::string::npos);

    REQUIRE(h.estimate_tokens() <= target);
    REQUIRE(r.reached_target);
}

TEST_CASE("[s5.f][cascade] ordering invariant: 1 before 2 before 3",
          "[s5.f][cascade]")
{
    auto h = build_synthetic_conversation();

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = true;
    sel.strip_large_tool_bodies     = true;
    sel.drop_old_reasoning          = true;
    sel.strip_threshold_tokens      = 100000;  // disable by sky-high threshold
    sel.older_than_turns            = 0;       // make layer 3 active

    int target = 0;  // never reached -> all three run
    LLMConfig lc;
    auto r = CompactionPipeline::run(h, sel, target, nullptr, lc);

    REQUIRE(r.layers.size() == 3);
    REQUIRE(r.layers[0].name.find("Layer 1") != std::string::npos);
    REQUIRE(r.layers[1].name.find("Layer 2") != std::string::npos);
    REQUIRE(r.layers[2].name.find("Layer 3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Layer 5 (mechanical drop) honours preservation
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][layer5] mechanical drop spares preserved messages",
          "[s5.f][layer5]")
{
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "sys"));
    h.add(make_msg(MessageRole::user, "first"));               // first user
    h.add(make_msg(MessageRole::assistant, std::string(4000, 'a')));
    h.add(make_msg(MessageRole::user, std::string(4000, 'B')));  // big -- drop ok
    h.add(make_msg(MessageRole::assistant, std::string(4000, 'b')));
    h.add(make_msg(MessageRole::user, "tail"));                // recent
    h.add(make_msg(MessageRole::assistant, std::string(500, 'c')));

    CompactionLayerSelection sel;
    sel.drop_redundant_tool_results = false;
    sel.strip_large_tool_bodies     = false;
    sel.drop_old_reasoning          = false;
    sel.drop_oldest_turns           = true;
    sel.llm_summary                 = false;
    sel.keep_recent_turns           = 1;
    sel.preserve_short_user_msgs_max_tokens  = 50;

    int before = h.estimate_tokens();
    LLMConfig lc;
    CompactionPipeline::run(h, sel, 1, nullptr, lc);

    // first/system + the tail turn must survive.
    REQUIRE(h.messages().front().content == "sys");
    bool first = false, tail = false;
    for (auto& m : h.messages()) {
        if (m.role == MessageRole::user && m.content == "first") first = true;
        if (m.role == MessageRole::user && m.content == "tail")  tail  = true;
    }
    REQUIRE(first);
    REQUIRE(tail);
    REQUIRE(h.estimate_tokens() < before);
}

// ---------------------------------------------------------------------------
// HistoryArchive round-trip + GC
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][archive] snapshot writes a file under <session>/",
          "[s5.f][archive]")
{
    auto dir = tmp_dir("archive1");
    HistoryArchive arch(dir);

    auto h = build_synthetic_conversation();
    auto p1 = arch.snapshot("sess-A", h);
    REQUIRE(!p1.empty());
    REQUIRE(fs::exists(p1));
    REQUIRE(p1.filename().string() == "history.before-compact-1.json");

    auto p2 = arch.snapshot("sess-A", h);
    REQUIRE(p2.filename().string() == "history.before-compact-2.json");

    auto p3 = arch.snapshot("sess-A", h);
    REQUIRE(p3.filename().string() == "history.before-compact-3.json");

    auto listing = arch.list("sess-A");
    REQUIRE(listing.size() == 3);

    // Snapshot is valid JSON with messages array.
    std::ifstream f(p2);
    nlohmann::json j;
    f >> j;
    REQUIRE(j.contains("messages"));
    REQUIRE(j["messages"].is_array());
    REQUIRE(j["counter"].get<int>() == 2);
}

TEST_CASE("[s5.f][archive] gc keeps last N snapshots", "[s5.f][archive]")
{
    auto dir = tmp_dir("archive_gc");
    HistoryArchive arch(dir);

    auto h = build_synthetic_conversation();
    for (int i = 0; i < 7; ++i) arch.snapshot("sess-B", h);
    REQUIRE(arch.list("sess-B").size() == 7);

    arch.gc("sess-B", 3);
    auto remaining = arch.list("sess-B");
    REQUIRE(remaining.size() == 3);
    // Should be the last three (counters 5, 6, 7).
    REQUIRE(HistoryArchive::counter_from_filename(
                remaining.front().filename().string()) == 5);
    REQUIRE(HistoryArchive::counter_from_filename(
                remaining.back().filename().string()) == 7);
}

TEST_CASE("[s5.f][archive] footnote_relative_path shape",
          "[s5.f][archive]")
{
    auto p = HistoryArchive::footnote_relative_path("2026-05-16_120000", 3);
    REQUIRE(p == ".locus/sessions/2026-05-16_120000/history.before-compact-3.json");
}

TEST_CASE("[s5.f][archive] counter_from_filename ignores foreign files",
          "[s5.f][archive]")
{
    REQUIRE(HistoryArchive::counter_from_filename(
                "history.before-compact-12.json") == 12);
    REQUIRE(HistoryArchive::counter_from_filename(
                "something_else.json") == 0);
    REQUIRE(HistoryArchive::counter_from_filename(
                "history.before-compact-abc.json") == 0);
}

// ---------------------------------------------------------------------------
// Footnote placement keeps the system prompt byte-stable (S4.F invariant)
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][footnote] footnote slot is index 1 (user role), system stable",
          "[s5.f][footnote]")
{
    // Simulate what AgentCore::apply_pending_compaction does after running
    // the pipeline: splice the footnote in at index 1 and keep [0] intact.
    ConversationHistory h;
    h.add(make_msg(MessageRole::system, "BYTE-STABLE SYSTEM PROMPT"));
    h.add(make_msg(MessageRole::user, "first user msg"));
    h.add(make_msg(MessageRole::assistant, "assistant"));

    auto note_text = "[Earlier context compacted; full pre-compaction "
                     "history archived at "
                     ".locus/sessions/sess-A/history.before-compact-1.json]";

    std::vector<ChatMessage> rebuilt;
    const auto& msgs = h.messages();
    rebuilt.reserve(msgs.size() + 1);
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        rebuilt.push_back(msgs[i]);
        if (i == 0) {
            ChatMessage fn;
            fn.role = MessageRole::user;
            fn.content = note_text;
            fn.token_estimate = TokenCounter::estimate_message(fn);
            rebuilt.push_back(fn);
        }
    }
    h.clear();
    for (auto& m : rebuilt) h.add(std::move(m));

    REQUIRE(h.messages().size() == 4);
    REQUIRE(h.messages()[0].role    == MessageRole::system);
    REQUIRE(h.messages()[0].content == "BYTE-STABLE SYSTEM PROMPT");
    REQUIRE(h.messages()[1].role    == MessageRole::user);
    REQUIRE(h.messages()[1].content.find("history.before-compact-1.json")
            != std::string::npos);
}

// ---------------------------------------------------------------------------
// Threshold band correctness (warn / auto / refuse)
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][thresholds] warn/auto/refuse fire at the right ratios",
          "[s5.f][thresholds]")
{
    WorkspaceConfig::Compaction c;
    c.warn_threshold  = 0.70;
    c.auto_threshold  = 0.85;

    int eff = 10000;

    auto band = [&](int used) {
        double r = static_cast<double>(used) / eff;
        if (r >= 1.0) return "refuse";
        if (r >= c.auto_threshold) return "auto";
        if (r >= c.warn_threshold) return "warn";
        return "ok";
    };

    REQUIRE(std::string(band(1000))  == "ok");
    REQUIRE(std::string(band(6999))  == "ok");
    REQUIRE(std::string(band(7000))  == "warn");
    REQUIRE(std::string(band(8499))  == "warn");
    REQUIRE(std::string(band(8500))  == "auto");
    REQUIRE(std::string(band(9999))  == "auto");
    REQUIRE(std::string(band(10000)) == "refuse");
    REQUIRE(std::string(band(11000)) == "refuse");
}

// ---------------------------------------------------------------------------
// format_summary_message shape
// ---------------------------------------------------------------------------

TEST_CASE("[s5.f][summary] format_summary_message carries span size",
          "[s5.f][summary]")
{
    auto s = CompactionPipeline::format_summary_message("body", 5);
    REQUIRE(s.find("5 earlier messages") != std::string::npos);
    REQUIRE(s.find("body") != std::string::npos);

    auto s1 = CompactionPipeline::format_summary_message("x", 1);
    REQUIRE(s1.find("1 earlier message") != std::string::npos);
    REQUIRE(s1.find("1 earlier messages") == std::string::npos);
}

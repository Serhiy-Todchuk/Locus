// Session save/restore -- round-trip parity test driven by a real LLM.
//
// Builds on the multi-tab harness used by test_int_tabs_and_sessions but
// focuses on the question: after a complete turn (user message -> assistant
// reasoning + tool calls + tool results -> final answer), does opening the
// saved session in a fresh AgentCore reproduce the same ChatMessage shape
// the live session had?
//
// The deterministic unit suite covers the on-disk JSON round-trip already
// (see tests/test_session_restore.cpp). This file's job is to assert that
// the LIVE state we feed to ConversationHistory after a real turn matches
// what we read back -- catching regressions where, say, a future refactor
// drops reasoning_content on the save path, or strips tool_call_id off
// the wire and breaks restore on the load side.

#include "harness_fixture.h"

#include "agent/conversation.h"
#include "core/locus_session.h"
#include "core/locus_tab.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

using namespace locus;
using namespace locus::integration;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

fs::path make_temp_workspace(const std::string& tag)
{
    auto unique = tag + "_" + std::to_string(std::random_device{}());
    auto root = fs::temp_directory_path() / ("locus_int_restore_" + unique);
    fs::create_directories(root / ".locus");

    std::ofstream f(root / ".locus" / "config.json");
    f << R"({
  "index": { "semantic_search": { "enabled": false } },
  "memory": { "enabled": false },
  "capabilities": {
    "background_processes": false,
    "semantic_search": false,
    "code_aware_search": false,
    "memory_bank": false,
    "web_retrieval": false
  }
})";
    // Drop a small file the model can read with the read_file tool. The
    // exact content is what the assistant should bring back in its reply.
    std::ofstream src(root / "marker.txt");
    src << "MARKER_TOKEN: persistedsaucer42\n";
    return root;
}

void best_effort_remove(const fs::path& p)
{
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        fs::remove_all(p, ec);
        if (!ec) return;
        std::this_thread::sleep_for(50ms);
    }
}

LLMConfig make_llm_config()
{
    LLMConfig cfg;
    if (const char* ep = std::getenv("LOCUS_INT_TEST_ENDPOINT"))
        cfg.base_url = ep;
    if (const char* mid = std::getenv("LOCUS_INT_TEST_MODEL"))
        cfg.model = mid;
    cfg.temperature = 0.2;
    cfg.max_tokens  = 1024;
    cfg.timeout_ms  = 180000;
    return cfg;
}

bool prompt_tab(LocusTab& tab, const std::string& msg,
                std::chrono::milliseconds timeout = 5min)
{
    HarnessFrontend fe;
    fe.attach_core(&tab.agent());
    tab.agent().register_frontend(&fe);
    fe.reset_turn_state();
    tab.agent().send_message(msg);
    bool ok = fe.wait_for_turn(timeout);
    tab.agent().unregister_frontend(&fe);
    return ok;
}

} // namespace

#ifdef _WIN32

TEST_CASE("Session restore: history shape (content + reasoning + tool_calls + "
          "tool_call_id) survives close+reopen",
          "[integration][llm][session-restore]")
{
    auto& h = harness();
    (void)h;  // touch so LM Studio reachability error fires early.

    auto root = make_temp_workspace("history");
    struct Cleanup { fs::path p; ~Cleanup() { best_effort_remove(p); } } cleanup{root};

    std::string session_id;
    std::vector<ChatMessage> baseline;

    // -- Pass 1: live session, drive one turn that exercises a tool call. ----
    {
        LocusSession session(root, make_llm_config());
        int idx = session.add_tab();
        REQUIRE(idx == 0);
        auto& tab = session.tab(idx);

        // Prompt is shaped so the assistant has to read marker.txt to answer.
        REQUIRE(prompt_tab(tab,
            "Use the read_file tool to read 'marker.txt' and then reply with "
            "the exact MARKER_TOKEN value from it. Reply format: "
            "'MARKER_TOKEN is <value>'."));

        REQUIRE_FALSE(tab.session_id().empty());
        session_id = tab.session_id();

        // Snapshot the in-memory history for comparison after the reload.
        for (const auto& m : tab.agent().history().messages())
            baseline.push_back(m);
    }

    // -- Pass 2: fresh LocusSession + LocusTab -> add_tab_for_session. -------
    LocusSession restored_session(root, make_llm_config());
    int idx = restored_session.add_tab_for_session(session_id);
    REQUIRE(idx >= 0);
    auto& tab2 = restored_session.tab(idx);
    REQUIRE(tab2.session_id() == session_id);

    const auto& restored = tab2.agent().history().messages();

    // We don't compare the leading system message byte-for-byte because the
    // system prompt rebuilds with the new workspace metadata. Everything else
    // (user / assistant / tool) must match.
    REQUIRE(restored.size() == baseline.size());
    for (size_t i = 0; i < baseline.size(); ++i) {
        INFO("message index " << i << " role=" << to_string(baseline[i].role));
        REQUIRE(restored[i].role == baseline[i].role);
        if (baseline[i].role == MessageRole::system) continue;

        REQUIRE(restored[i].content          == baseline[i].content);
        REQUIRE(restored[i].reasoning_content == baseline[i].reasoning_content);
        REQUIRE(restored[i].tool_call_id     == baseline[i].tool_call_id);
        REQUIRE(restored[i].tool_calls.size() == baseline[i].tool_calls.size());
        for (size_t k = 0; k < baseline[i].tool_calls.size(); ++k) {
            REQUIRE(restored[i].tool_calls[k].id        == baseline[i].tool_calls[k].id);
            REQUIRE(restored[i].tool_calls[k].name      == baseline[i].tool_calls[k].name);
            // arguments is a raw JSON string. Some servers normalise whitespace,
            // some don't -- compare as a string to catch a real change without
            // being misled by formatting.
            REQUIRE(restored[i].tool_calls[k].arguments == baseline[i].tool_calls[k].arguments);
        }
    }

    // Sanity: at least one tool turn happened, otherwise the test isn't
    // actually exercising the round-trip.
    bool had_tool_call = false;
    bool had_tool_result = false;
    for (const auto& m : restored) {
        if (m.role == MessageRole::assistant && !m.tool_calls.empty())
            had_tool_call = true;
        if (m.role == MessageRole::tool && !m.tool_call_id.empty())
            had_tool_result = true;
    }
    REQUIRE(had_tool_call);
    REQUIRE(had_tool_result);
}

#endif  // _WIN32

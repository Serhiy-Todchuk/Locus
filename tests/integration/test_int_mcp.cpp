// S4.G -- MCP client integration test.
//
// Spins up a self-contained LocusSession on a temp workspace whose mcp.json
// points at the bundled locus_mock_mcp_server. The mock advertises an `echo`
// tool that returns its `text` argument verbatim. We then ask the live LLM
// to call `mcp:mock:echo` and verify it routes end-to-end:
//
//   LLM -> ToolDispatcher -> McpTool::execute -> McpClient -> stdio
//   -> mock server -> response -> McpClient -> McpTool -> chat
//
// We deliberately do NOT augment the shared IntegrationHarness with an
// McpManager: the harness opens WS1 (the Locus repo) and adding MCP servers
// to WS1's mcp.json would pollute the dev environment, while bolting an
// inline MCP server onto the shared registry would risk dangling McpClient
// pointers in McpTool entries that subsequent tests still see. A local
// LocusSession owns its own ToolRegistry + McpManager and tears them down
// in the right order at scope end (McpTools die before McpClients).
//
// Small/local models are not always cooperative -- if the model fails to
// invoke the namespaced MCP tool we soft-skip with WARN rather than fail
// the suite, matching the convention from test_int_plan.cpp.

#include "harness_fixture.h"

#include "core/locus_session.h"
#include "mcp/mcp_manager.h"
#include "tools/tool.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace locus;
using namespace locus::integration;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

namespace {

namespace fs = std::filesystem;

// Locate the bundled mock MCP server. The CMake target sets
// LOCUS_MOCK_MCP_SERVER_PATH to the absolute path at build time so the
// integration exe can spawn it regardless of where it was launched from.
fs::path mock_server_path()
{
#ifdef LOCUS_MOCK_MCP_SERVER_PATH
    return fs::path(LOCUS_MOCK_MCP_SERVER_PATH);
#else
    return {};
#endif
}

// Write a temp workspace with a minimal mcp.json + a config.json that
// disables semantic search (we don't need an embedder for this test, and
// loading bge-m3 here would add seconds to each run).
fs::path make_temp_workspace_with_mock()
{
    auto unique = std::to_string(std::random_device{}());
    auto root = fs::temp_directory_path() / ("locus_int_mcp_" + unique);
    fs::create_directories(root / ".locus");

    {
        std::ofstream f(root / ".locus" / "config.json");
        f << R"({
  "index": { "semantic_search": { "enabled": false } }
})";
    }
    {
        std::ofstream f(root / ".locus" / "mcp.json");
        f << R"({"mcpServers":{"mock":{"command":")"
          << mock_server_path().generic_string()
          << R"("}}})";
    }
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

bool wait_for_turn_done(HarnessFrontend& fe, std::chrono::milliseconds timeout)
{
    return fe.wait_for_turn(timeout);
}

} // namespace

#ifdef _WIN32

TEST_CASE("MCP echo tool round-trips via the agent loop",
          "[integration][llm][mcp]")
{
    // Ensure LM Studio is reachable + grab the shared LLM config.
    auto& h = harness();

    auto mock = mock_server_path();
    REQUIRE_FALSE(mock.empty());
    REQUIRE(fs::exists(mock));

    auto root = make_temp_workspace_with_mock();

    // Tear-down guard. Run before LocusSession destruction in case of REQUIRE
    // bail-out -- fs::remove_all without the session torn down would race
    // against the still-running mock child holding cwd.
    struct Cleanup {
        fs::path root;
        ~Cleanup() { best_effort_remove(root); }
    };

    {
        // Build our own LLMConfig from env so the test talks to whichever
        // LM Studio instance the user has running. LocusSession will probe
        // for context length / model id on its own (same as in production).
        LLMConfig cfg;
        if (const char* ep = std::getenv("LOCUS_INT_TEST_ENDPOINT"))
            cfg.base_url = ep;
        if (const char* mid = std::getenv("LOCUS_INT_TEST_MODEL"))
            cfg.model = mid;
        cfg.temperature = 0.2;
        cfg.max_tokens  = 2048;
        cfg.timeout_ms  = 120000;

        LocusSession session(root, cfg);

        // The mock server's `echo` tool is namespaced as mcp:mock:echo. It
        // must be visible in the registry after LocusSession ctor returns.
        ITool* echo = session.tools().find("mcp:mock:echo");
        REQUIRE(echo != nullptr);

        // Tools live behind the approval gate. The harness frontend
        // auto-approves on `on_tool_call_pending` regardless of policy
        // (see harness_frontend.cpp), but the default `ask` still means
        // the dispatcher emits the pending event we observe -- exactly
        // what we want for assertions.
        HarnessFrontend fe;
        fe.attach_core(&session.agent());
        session.agent().register_frontend(&fe);

        fe.reset_turn_state();
        session.agent().send_message(
            "Use the mcp:mock:echo tool with argument text=\"banana-42\". "
            "Do not paraphrase, do not summarise. Call exactly that tool, "
            "exactly once, with exactly that text argument.");

        // Local 7-9B class models can take a couple of minutes on fresh
        // prefill; mirror test_int_plan.cpp's budget.
        bool finished = wait_for_turn_done(fe, std::chrono::minutes(3));
        INFO("tokens: " << fe.tokens());

        // Defensive teardown order: detach the frontend BEFORE LocusSession
        // destruction so a late event firing on the agent thread can't hit
        // a half-dead HarnessFrontend on stack unwind.
        session.agent().unregister_frontend(&fe);

        REQUIRE(finished);

        // Bind tool_calls() / tool_results() to named locals -- both return
        // by value, so iterating directly over the rvalue and saving a
        // pointer into it would dangle the moment the range-for ends.
        const auto tool_calls   = fe.tool_calls();
        const auto tool_results = fe.tool_results();

        // Was the MCP tool actually called?
        const ObservedToolCall* call = nullptr;
        for (auto& c : tool_calls) {
            INFO("tool seen: " << c.tool_name);
            if (c.tool_name == "mcp:mock:echo") { call = &c; break; }
        }
        if (!call) {
            WARN("Model did not invoke mcp:mock:echo. Likely small-model "
                 "tool-selection variance; protocol mechanics are covered "
                 "by tests/test_mcp.cpp at unit level. Skipping the "
                 "round-trip assertions.");
            Cleanup _c{root};
            return;
        }

        // Argument shape -- the model should have passed text="banana-42".
        INFO("call args: " << call->args.dump());
        REQUIRE(call->args.contains("text"));
        REQUIRE_THAT(call->args["text"].get<std::string>(),
                     ContainsSubstring("banana-42"));

        // Result must contain the echoed text. McpTool::execute flattens
        // result.content[] into a plain string for both `content` and
        // `display`; ObservedToolResult captures `display`.
        const ObservedToolResult* result = nullptr;
        for (auto& r : tool_results) {
            if (r.call_id == call->id) { result = &r; break; }
        }
        REQUIRE(result != nullptr);
        REQUIRE_THAT(result->display, ContainsSubstring("banana-42"));
    }

    Cleanup _c{root};
}

#endif // _WIN32

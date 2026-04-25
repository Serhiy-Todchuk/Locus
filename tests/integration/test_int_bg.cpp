#include "harness_fixture.h"

#include "process_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace locus::integration;
using namespace std::chrono_literals;

// S4.I — background command tools. These use the LLM to chain run_command_bg
// → read_process_output / list_processes / stop_process in a single turn.
// Small models (Gemma 4 E4B) tend to call them sequentially within one
// response, which is exactly the behaviour we want to validate.

TEST_CASE("agent starts a background process and reads its output",
          "[integration][llm][bg]")
{
    auto& h = harness();

    PromptResult r = h.prompt(
        "Use the run_command_bg tool to start the shell command "
        "`cmd /c echo unique-bg-marker-7421`. The tool will return a "
        "process_id. Wait a moment, then use the read_process_output tool "
        "with that process_id to read what the command printed. Tell me "
        "exactly what was printed.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("run_command_bg"));
    REQUIRE(r.tool_called("read_process_output"));

    const auto* read_res = r.find_result_for("read_process_output");
    REQUIRE(read_res != nullptr);
    REQUIRE(read_res->display.find("unique-bg-marker-7421") != std::string::npos);
}

TEST_CASE("agent lists active background processes", "[integration][llm][bg]")
{
    auto& h = harness();

    // Seed the registry directly so list_processes has something to show
    // regardless of which earlier tests ran. Echoes finish nearly instantly
    // but the registry retains exited entries.
    auto* reg = h.workspace().processes();
    REQUIRE(reg != nullptr);
    int seed_id = reg->spawn("echo seeded-process-marker");
    REQUIRE(seed_id > 0);
    std::this_thread::sleep_for(200ms);  // let the echo finish

    PromptResult r = h.prompt(
        "Use the list_processes tool to show me every background process "
        "currently tracked by this workspace. Report each process_id and its "
        "status.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("list_processes"));

    const auto* list_res = r.find_result_for("list_processes");
    REQUIRE(list_res != nullptr);
    // The seeded process must appear in the listing.
    REQUIRE(list_res->display.find("echo seeded-process-marker") != std::string::npos);
}

TEST_CASE("agent stops a long-running background process",
          "[integration][llm][bg]")
{
    auto& h = harness();

    PromptResult r = h.prompt(
        "Use the run_command_bg tool to start the shell command "
        "`cmd /c ping -n 30 -w 1000 127.0.0.1` in the background. It returns "
        "a process_id. Then immediately use the stop_process tool with that "
        "process_id to terminate it.");

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_called("run_command_bg"));
    REQUIRE(r.tool_called("stop_process"));

    // Verify directly via the registry that the process did get killed —
    // the LLM may have invented a process_id, so we check the most recent
    // entry actually terminated.
    auto* reg = h.workspace().processes();
    REQUIRE(reg != nullptr);

    // Give the OS a beat to drain the reader thread after termination.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    bool any_killed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto entries = reg->list();
        for (const auto& e : entries) {
            if (e.command.find("ping") != std::string::npos &&
                e.status == locus::BackgroundProcess::Status::killed) {
                any_killed = true;
                break;
            }
        }
        if (any_killed) break;
        std::this_thread::sleep_for(100ms);
    }
    REQUIRE(any_killed);
}

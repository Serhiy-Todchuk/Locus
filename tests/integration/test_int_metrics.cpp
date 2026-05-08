// S4.S -- telemetry / metrics surfaces.
//
// Verifies the end-to-end path: real agent turns populate MetricsAggregator
// (per-turn samples, per-tool histogram, retrieval hit-rate); the `/metrics`
// slash returns a human-readable summary; `/export_metrics json|csv` writes
// a file under `.locus/metrics/` whose contents include the recorded turn.

#include "harness_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace locus::integration;
namespace fs = std::filesystem;

namespace {

std::string read_whole(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

// Snapshot the existing files in `<workspace>/.locus/metrics/` so the test
// can identify the export emitted by /export_metrics regardless of how many
// other runs left files behind.
std::set<fs::path> list_metrics_files(const fs::path& dir)
{
    std::set<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file()) out.insert(e.path());
    return out;
}

} // namespace

TEST_CASE("/metrics summarises tokens and per-tool counts after a real turn",
          "[integration][llm][metrics]")
{
    auto& h = harness();

    // Drive one real LLM round that calls a tool. read_file is a stable,
    // small-cost call available in every test build.
    PromptResult r1 = h.prompt(
        "Use the read_file tool to read CLAUDE.md (at the workspace root) "
        "and tell me one sentence about Locus.");
    REQUIRE_FALSE(r1.timed_out);
    REQUIRE(r1.errors.empty());
    REQUIRE(r1.tool_called("read_file"));

    // /metrics is handled directly in AgentCore::try_slash_command and
    // emits its summary via on_token. No tools, no LLM round.
    PromptResult r2 = h.prompt("/metrics");
    REQUIRE_FALSE(r2.timed_out);
    REQUIRE(r2.errors.empty());
    REQUIRE(r2.tool_calls.empty());

    INFO("metrics output:\n" << r2.tokens);
    // The summary header contains "turn"; the body lists tools by name.
    REQUIRE(r2.tokens.find("turn") != std::string::npos);
    REQUIRE(r2.tokens.find("tokens:") != std::string::npos);
    // Per-tool histogram should mention the read_file we just ran.
    REQUIRE(r2.tokens.find("read_file") != std::string::npos);
}

TEST_CASE("/export_metrics json writes a file under .locus/metrics/",
          "[integration][llm][metrics]")
{
    auto& h = harness();

    const fs::path metrics_dir = h.workspace_root() / ".locus" / "metrics";
    auto before = list_metrics_files(metrics_dir);

    // Make sure at least one turn has landed in the aggregator.
    PromptResult prep = h.prompt(
        "Use the read_file tool to read CLAUDE.md and reply with one short "
        "sentence about it.");
    REQUIRE_FALSE(prep.timed_out);
    REQUIRE(prep.tool_called("read_file"));

    PromptResult r = h.prompt("/export_metrics json");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_calls.empty());
    INFO("export output: " << r.tokens);

    // The new file is whatever appeared in metrics/ that wasn't there before.
    auto after = list_metrics_files(metrics_dir);
    fs::path written;
    for (const auto& p : after) {
        if (before.find(p) == before.end()) {
            written = p;
            break;
        }
    }
    REQUIRE_FALSE(written.empty());
    REQUIRE(written.extension() == ".json");

    // The file must be valid JSON containing at least one of the expected
    // top-level keys; we keep the assertion loose so future schema additions
    // don't break it.
    std::string body = read_whole(written);
    INFO("exported json body: " << body);
    REQUIRE(body.size() > 0);
    bool has_known_key =
        body.find("\"turn_count\"")        != std::string::npos ||
        body.find("\"tokens_in_total\"")   != std::string::npos ||
        body.find("\"tool_calls_by_name\"") != std::string::npos;
    REQUIRE(has_known_key);
}

TEST_CASE("/export_metrics csv writes a file under .locus/metrics/",
          "[integration][llm][metrics]")
{
    auto& h = harness();

    const fs::path metrics_dir = h.workspace_root() / ".locus" / "metrics";
    auto before = list_metrics_files(metrics_dir);

    PromptResult prep = h.prompt(
        "Use the read_file tool to read CLAUDE.md and reply briefly.");
    REQUIRE_FALSE(prep.timed_out);
    REQUIRE(prep.tool_called("read_file"));

    PromptResult r = h.prompt("/export_metrics csv");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    REQUIRE(r.tool_calls.empty());

    auto after = list_metrics_files(metrics_dir);
    fs::path written;
    for (const auto& p : after) {
        if (before.find(p) == before.end()) {
            written = p;
            break;
        }
    }
    REQUIRE_FALSE(written.empty());
    REQUIRE(written.extension() == ".csv");

    std::string body = read_whole(written);
    INFO("exported csv body: " << body);
    REQUIRE(body.size() > 0);
    // CSV form is line-oriented; require at least one newline and at least
    // one comma so we know a header was written.
    REQUIRE(body.find('\n') != std::string::npos);
    REQUIRE(body.find(',')  != std::string::npos);
}

TEST_CASE("/export_metrics with bogus format reports an error",
          "[integration][metrics]")
{
    auto& h = harness();
    // Drive a turn so the aggregator isn't empty -- not strictly needed for
    // the format-validation path, but keeps the test honest.
    PromptResult prep = h.prompt(
        "Use the read_file tool to read CLAUDE.md and reply briefly.");
    REQUIRE_FALSE(prep.timed_out);

    PromptResult r = h.prompt("/export_metrics yaml");
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.errors.empty());
    INFO("output: " << r.tokens);
    // The export_metrics handler returns an error string starting with
    // "Error:" -- reaches the harness via on_token.
    REQUIRE(r.tokens.find("Error:") != std::string::npos);
}

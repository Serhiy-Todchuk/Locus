#include <catch2/catch_test_macros.hpp>

#include "agent_core.h"
#include "frontend.h"
#include "tool_registry.h"
#include "tools.h"
#include "support/fake_workspace_services.h"

#include <filesystem>
#include <optional>

using namespace locus;

namespace {

// Minimal ILLMClient that never streams — AgentCore is exercised here only
// for its attached-context state and system-prompt composition; we never
// actually start the agent thread, so no LLM call is made.
class NullLLM : public ILLMClient {
public:
    void stream_completion(const std::vector<ChatMessage>&,
                           const std::vector<ToolSchema>&,
                           const StreamCallbacks&) override {}
    ModelInfo query_model_info() override { return {}; }
};

// Captures on_attached_context_changed broadcasts so the round-trip test
// can verify both the Core API result and the frontend fan-out.
class CapturingFrontend : public IFrontend {
public:
    void on_turn_start() override {}
    void on_token(std::string_view) override {}
    void on_tool_call_pending(const ToolCall&, const std::string&) override {}
    void on_tool_result(const std::string&, const std::string&) override {}
    void on_turn_complete() override {}
    void on_context_meter(int, int) override {}
    void on_compaction_needed(int, int) override {}
    void on_session_reset() override {}
    void on_error(const std::string&) override {}
    void on_embedding_progress(int, int) override {}
    void on_activity(const ActivityEvent&) override {}
    void on_attached_context_changed(
        const std::optional<AttachedContext>& ctx) override
    {
        last = ctx;
        ++calls;
    }
    std::optional<AttachedContext> last;
    int calls = 0;
};

AgentCore make_core()
{
    static NullLLM      llm;
    static ToolRegistry reg;
    static bool         registered = false;
    if (!registered) {
        register_builtin_tools(reg);
        registered = true;
    }
    // index/embedder/workspace deliberately null — outline lookup is skipped.
    static test::FakeWorkspaceServices services{std::filesystem::temp_directory_path()};
    WorkspaceMetadata meta;
    meta.root = services.root();
    LLMConfig cfg;
    return AgentCore(llm, reg, services, /*locus_md=*/"", meta, cfg,
                     std::filesystem::temp_directory_path());
}

} // namespace

TEST_CASE("Attached context: round-trip through Core", "[s2.4]")
{
    auto core = make_core();

    REQUIRE_FALSE(core.attached_context().has_value());

    AttachedContext ctx{"src/main.cpp", "entry point"};
    core.set_attached_context(ctx);

    auto got = core.attached_context();
    REQUIRE(got.has_value());
    CHECK(got->file_path == "src/main.cpp");
    CHECK(got->preview   == "entry point");

    core.clear_attached_context();
    REQUIRE_FALSE(core.attached_context().has_value());
}

TEST_CASE("Attached context: frontends are notified", "[s2.4]")
{
    auto core = make_core();
    CapturingFrontend fe;
    core.register_frontend(&fe);

    core.set_attached_context({"docs/README.md", ""});
    REQUIRE(fe.calls == 1);
    REQUIRE(fe.last.has_value());
    CHECK(fe.last->file_path == "docs/README.md");

    core.clear_attached_context();
    CHECK(fe.calls == 2);
    CHECK_FALSE(fe.last.has_value());

    // clear when already empty must not refire (would spam the chip widget).
    core.clear_attached_context();
    CHECK(fe.calls == 2);

    core.unregister_frontend(&fe);
}

TEST_CASE("Attached context: injected into system prompt", "[s2.4]")
{
    auto core = make_core();

    // Before attach: no marker.
    const auto& before = core.history().messages().front().content;
    REQUIRE(before.find("[Attached:") == std::string::npos);

    core.set_attached_context({"include/widget.h", ""});

    // The seed system message (index 0) must reflect the attachment in place.
    const auto& after = core.history().messages().front().content;
    REQUIRE(after.find("## Attached File") != std::string::npos);
    REQUIRE(after.find("[Attached: include/widget.h]") != std::string::npos);

    core.clear_attached_context();

    const auto& cleared = core.history().messages().front().content;
    REQUIRE(cleared.find("[Attached:")      == std::string::npos);
    REQUIRE(cleared.find("## Attached File") == std::string::npos);
}

TEST_CASE("Attached context: survives reset_conversation", "[s2.4]")
{
    auto core = make_core();
    core.set_attached_context({"path/to/file.txt", ""});

    core.reset_conversation();

    // Pin must persist; system message must still carry the marker.
    REQUIRE(core.attached_context().has_value());
    const auto& sys = core.history().messages().front().content;
    REQUIRE(sys.find("[Attached: path/to/file.txt]") != std::string::npos);
}

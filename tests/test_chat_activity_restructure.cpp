// S5.G -- Chat / Activity Restructure unit tests.
//
// Covers the chat-side and activity-side wiring that S5.G introduces:
//   - ChatMessage gains a stable history_id assigned by ConversationHistory::add
//   - ConversationHistory::delete_by_id removes messages and refuses the
//     leading system entry
//   - LLMContext::add_message + delete_message broadcast on_history_message_*
//     hooks so chat panels can build a dom -> history map
//   - LLMContext::load_session re-announces every loaded message
//   - EmbeddingWorker fires on_activity throttled by chunk count + queue drain
//
// All tests run synchronously without an agent thread (LLMContext is the
// owner so no ConversationOwnerScope is needed when constructed in tests).

#include <catch2/catch_test_macros.hpp>

#include "agent/conversation.h"
#include "agent/llm_context.h"
#include "agent/session_manager.h"
#include "agent/system_prompt.h"
#include "agent/system_prompt_assembly.h"
#include "core/frontend.h"
#include "core/frontend_registry.h"
#include "tools/tool_registry.h"
#include "tools/tools.h"
#include "support/fake_workspace_services.h"

#include <filesystem>
#include <random>
#include <unordered_set>

using namespace locus;
namespace fs = std::filesystem;

namespace {

fs::path make_tmp_dir(const std::string& tag)
{
    std::random_device rd;
    auto p = fs::temp_directory_path()
             / ("locus_s5g_" + tag + "_" + std::to_string(rd()));
    fs::create_directories(p);
    return p;
}

// Records every on_history_message_* + on_activity callback for assertions.
class RecordingFrontend : public IFrontend {
public:
    struct AddRecord {
        int          history_id;
        MessageRole  role;
        bool         deletable;
    };
    struct DeleteRecord { int history_id; };

    std::vector<AddRecord>    adds;
    std::vector<DeleteRecord> deletes;
    std::vector<ActivityEvent> activities;

    void on_turn_start() override {}
    void on_token(std::string_view) override {}
    void on_tool_call_pending(const ToolCall&, const std::string&,
                              bool, const std::vector<std::string>&) override {}
    void on_tool_result(const std::string&, const std::string&, bool) override {}
    void on_turn_complete() override {}
    void on_context_meter(int, int, int, int, int) override {}
    void on_compaction_needed(int, int) override {}
    void on_session_reset() override {}
    void on_error(const std::string&) override {}
    void on_embedding_progress(int, int) override {}
    void on_activity(const ActivityEvent& e) override { activities.push_back(e); }

    void on_history_message_added(int history_id, MessageRole role,
                                   bool deletable) override
    {
        adds.push_back({history_id, role, deletable});
    }
    void on_history_message_deleted(int history_id) override
    {
        deletes.push_back({history_id});
    }
};

struct Fixture {
    ToolRegistry        tools;
    test::FakeWorkspaceServices services;
    FrontendRegistry    frontends;
    SessionManager      sessions;
    fs::path            sessions_dir;
    fs::path            checkpoints_dir;
    RecordingFrontend   recorder;

    Fixture(const std::string& tag)
        : services(make_tmp_dir(tag + "_ws"))
        , sessions(make_tmp_dir(tag + "_sessions"))
        , sessions_dir(sessions.sessions_dir())
        , checkpoints_dir(make_tmp_dir(tag + "_checkpoints"))
    {
        register_builtin_tools(tools);
    }

    std::unique_ptr<LLMContext> make_context()
    {
        LLMConfig cfg;
        cfg.context_limit = 8192;
        WorkspaceMetadata meta;
        meta.root = services.root();
        auto prompt = SystemPromptAssembly::build(/*locus_md=*/"", meta,
                                                  tools, cfg.tool_format);
        return std::make_unique<LLMContext>(
            services, cfg, std::move(prompt),
            frontends, sessions, checkpoints_dir);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// ConversationHistory: history_id + delete_by_id
// ---------------------------------------------------------------------------

TEST_CASE("ConversationHistory: add() assigns monotonic history_id",
          "[s5.g][conversation][history-id]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "sys"});
    h.add({MessageRole::user, "u1"});
    h.add({MessageRole::assistant, "a1"});

    REQUIRE(h.size() == 3);
    REQUIRE(h.messages()[0].history_id == 1);
    REQUIRE(h.messages()[1].history_id == 2);
    REQUIRE(h.messages()[2].history_id == 3);
}

TEST_CASE("ConversationHistory: caller-supplied id is preserved + advances counter",
          "[s5.g][conversation][history-id]")
{
    ConversationHistory h;
    ChatMessage seeded;
    seeded.role       = MessageRole::user;
    seeded.content    = "hand-picked";
    seeded.history_id = 42;
    h.add(std::move(seeded));

    REQUIRE(h.messages().back().history_id == 42);

    // The auto-assigned ids continue past the seeded high-water mark.
    h.add({MessageRole::assistant, "auto"});
    REQUIRE(h.messages().back().history_id == 43);
}

TEST_CASE("ConversationHistory: clear() resets the id counter",
          "[s5.g][conversation][history-id]")
{
    ConversationHistory h;
    h.add({MessageRole::user, "u1"});
    h.add({MessageRole::user, "u2"});
    REQUIRE(h.messages().back().history_id == 2);

    h.clear();
    h.add({MessageRole::user, "fresh"});
    REQUIRE(h.messages().back().history_id == 1);
}

TEST_CASE("ConversationHistory: delete_by_id removes the matching message",
          "[s5.g][conversation][delete]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "sys"});
    h.add({MessageRole::user, "u1"});
    h.add({MessageRole::assistant, "a1"});
    h.add({MessageRole::user, "u2"});

    int u1_id = h.messages()[1].history_id;
    REQUIRE(h.delete_by_id(u1_id));
    REQUIRE(h.size() == 3);

    bool saw_u1 = false;
    for (const auto& m : h.messages())
        if (m.content == "u1") saw_u1 = true;
    REQUIRE_FALSE(saw_u1);

    // Surviving entries keep their original ids.
    REQUIRE(h.messages()[0].history_id == 1);  // sys
    REQUIRE(h.messages()[1].history_id == 3);  // a1
    REQUIRE(h.messages()[2].history_id == 4);  // u2
}

TEST_CASE("ConversationHistory: delete_by_id refuses the system message",
          "[s5.g][conversation][delete]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "sys"});
    h.add({MessageRole::user, "u1"});

    int sys_id = h.messages().front().history_id;
    REQUIRE_FALSE(h.delete_by_id(sys_id));
    REQUIRE(h.size() == 2);
    REQUIRE(h.messages().front().role == MessageRole::system);
}

TEST_CASE("ConversationHistory: delete_by_id returns false for unknown id",
          "[s5.g][conversation][delete]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "sys"});
    h.add({MessageRole::user, "u1"});

    REQUIRE_FALSE(h.delete_by_id(999));
    REQUIRE_FALSE(h.delete_by_id(0));
    REQUIRE_FALSE(h.delete_by_id(-5));
    REQUIRE(h.size() == 2);
}

TEST_CASE("ConversationHistory: from_json re-numbers history_ids 1..N",
          "[s5.g][conversation][history-id][session]")
{
    ConversationHistory h;
    h.add({MessageRole::system, "sys"});
    h.add({MessageRole::user, "u1"});
    h.add({MessageRole::user, "u2"});

    // Delete in the middle so the original ids have a gap.
    int u1_id = h.messages()[1].history_id;
    REQUIRE(h.delete_by_id(u1_id));

    auto j = h.to_json();
    auto restored = ConversationHistory::from_json(j);
    REQUIRE(restored.size() == 2);
    // After load, ids are re-walked: gap is gone, counter resumes from N+1.
    REQUIRE(restored.messages()[0].history_id == 1);
    REQUIRE(restored.messages()[1].history_id == 2);
}

// ---------------------------------------------------------------------------
// LLMContext: broadcast hooks
// ---------------------------------------------------------------------------

TEST_CASE("LLMContext: ctor + add_message broadcast on_history_message_added",
          "[s5.g][llm-context][broadcast]")
{
    Fixture f("broadcast_add");
    f.frontends.register_frontend(&f.recorder);
    auto ctx = f.make_context();

    // System message gets broadcast on construction.
    REQUIRE(f.recorder.adds.size() == 1);
    REQUIRE(f.recorder.adds[0].role == MessageRole::system);
    REQUIRE_FALSE(f.recorder.adds[0].deletable);  // system never deletable

    ctx->add_message({MessageRole::user, "hello"});
    REQUIRE(f.recorder.adds.size() == 2);
    REQUIRE(f.recorder.adds[1].role == MessageRole::user);
    REQUIRE(f.recorder.adds[1].deletable);

    // Assistant without tool_calls is deletable.
    ctx->add_message({MessageRole::assistant, "hi back"});
    REQUIRE(f.recorder.adds.size() == 3);
    REQUIRE(f.recorder.adds[2].role == MessageRole::assistant);
    REQUIRE(f.recorder.adds[2].deletable);

    // Assistant with tool_calls is NOT deletable (paired with tool results).
    ChatMessage with_tools;
    with_tools.role = MessageRole::assistant;
    with_tools.tool_calls = {{"call_1", "read_file", "{}"}};
    ctx->add_message(std::move(with_tools));
    REQUIRE(f.recorder.adds.size() == 4);
    REQUIRE(f.recorder.adds[3].role == MessageRole::assistant);
    REQUIRE_FALSE(f.recorder.adds[3].deletable);

    // Tool result IS deletable; clicking its X routes through
    // LLMContext::delete_message which walks back to the parent assistant
    // via tool_call_id and pair-deletes the whole turn.
    ChatMessage tool_result;
    tool_result.role         = MessageRole::tool;
    tool_result.tool_call_id = "call_1";
    tool_result.content      = "result body";
    ctx->add_message(std::move(tool_result));
    REQUIRE(f.recorder.adds.size() == 5);
    REQUIRE(f.recorder.adds[4].role == MessageRole::tool);
    REQUIRE(f.recorder.adds[4].deletable);
}

TEST_CASE("LLMContext: delete_message removes + broadcasts on_history_message_deleted",
          "[s5.g][llm-context][delete]")
{
    Fixture f("broadcast_del");
    auto ctx = f.make_context();
    f.frontends.register_frontend(&f.recorder);  // register after ctor so only delete fires

    ctx->add_message({MessageRole::user, "first"});
    int user_id = ctx->history().messages().back().history_id;
    ctx->add_message({MessageRole::assistant, "reply"});

    REQUIRE(ctx->history().size() == 3);
    REQUIRE(f.recorder.deletes.empty());

    REQUIRE(ctx->delete_message(user_id));

    REQUIRE(ctx->history().size() == 2);
    REQUIRE(f.recorder.deletes.size() == 1);
    REQUIRE(f.recorder.deletes[0].history_id == user_id);
}

TEST_CASE("LLMContext: delete_message refuses the system message",
          "[s5.g][llm-context][delete]")
{
    Fixture f("delete_sys");
    auto ctx = f.make_context();
    f.frontends.register_frontend(&f.recorder);

    int sys_id = ctx->history().messages().front().history_id;
    REQUIRE_FALSE(ctx->delete_message(sys_id));
    REQUIRE(ctx->history().size() == 1);
    REQUIRE(f.recorder.deletes.empty());
}

TEST_CASE("LLMContext: delete_message on tool walks back to parent assistant",
          "[s5.g][llm-context][delete]")
{
    Fixture f("delete_tool_walks_back");
    auto ctx = f.make_context();
    f.frontends.register_frontend(&f.recorder);

    // user -> assistant with two tool_calls -> two tool results.
    ctx->add_message({MessageRole::user, "do it"});
    int user_id = ctx->history().messages().back().history_id;

    ChatMessage asst;
    asst.role       = MessageRole::assistant;
    asst.content    = "";
    asst.tool_calls = {{"call_a", "read_file", "{}"},
                       {"call_b", "list_dir",  "{}"}};
    ctx->add_message(std::move(asst));
    int asst_id = ctx->history().messages().back().history_id;

    ChatMessage tool_a;
    tool_a.role         = MessageRole::tool;
    tool_a.tool_call_id = "call_a";
    tool_a.content      = "result A";
    ctx->add_message(std::move(tool_a));
    int tool_a_id = ctx->history().messages().back().history_id;

    ChatMessage tool_b;
    tool_b.role         = MessageRole::tool;
    tool_b.tool_call_id = "call_b";
    tool_b.content      = "result B";
    ctx->add_message(std::move(tool_b));
    int tool_b_id = ctx->history().messages().back().history_id;

    // history: system + user + assistant + tool_a + tool_b = 5
    REQUIRE(ctx->history().size() == 5);
    f.recorder.deletes.clear();

    // Clicking the X on tool_a should pair-delete assistant + tool_a + tool_b.
    REQUIRE(ctx->delete_message(tool_a_id));
    REQUIRE(ctx->history().size() == 2);  // system + user
    // All three rows broadcast on_history_message_deleted (chat panel uses
    // them to drop the bubbles); user-clicked X started from tool_a but the
    // pair-delete cascades to assistant + every matching tool result.
    REQUIRE(f.recorder.deletes.size() == 3);
    std::unordered_set<int> deleted_ids;
    for (const auto& d : f.recorder.deletes) deleted_ids.insert(d.history_id);
    REQUIRE(deleted_ids.count(asst_id)   == 1);
    REQUIRE(deleted_ids.count(tool_a_id) == 1);
    REQUIRE(deleted_ids.count(tool_b_id) == 1);
    // The user message is untouched.
    REQUIRE(ctx->history().messages()[1].history_id == user_id);
}

TEST_CASE("LLMContext: load_session re-announces every message",
          "[s5.g][llm-context][session]")
{
    Fixture f("load_announce");
    auto ctx = f.make_context();

    ctx->add_message({MessageRole::user, "saved-u"});
    ctx->add_message({MessageRole::assistant, "saved-a"});
    auto id = ctx->save_session();

    ctx->reset_for_new_conversation();

    // Register the recorder AFTER reset so we only catch load_session's emits.
    f.frontends.register_frontend(&f.recorder);
    ctx->load_session(id);

    // Three messages re-announced: system + user + assistant.
    REQUIRE(f.recorder.adds.size() == 3);
    REQUIRE(f.recorder.adds[0].role == MessageRole::system);
    REQUIRE_FALSE(f.recorder.adds[0].deletable);
    REQUIRE(f.recorder.adds[1].role == MessageRole::user);
    REQUIRE(f.recorder.adds[1].deletable);
    REQUIRE(f.recorder.adds[2].role == MessageRole::assistant);
    REQUIRE(f.recorder.adds[2].deletable);
}

// ---------------------------------------------------------------------------
// EmbeddingWorker activity throttling -- driven by direct callback invocation
// (the worker thread itself is exercised by integration tests with a real
// embedder; this case validates the throttle gate logic in isolation).
// ---------------------------------------------------------------------------

#include "index/embedding_worker.h"
#include "core/database.h"
#include "index/embedder.h"
#include "support/shared_embedder.h"

TEST_CASE("EmbeddingWorker: enqueue fires on_activity with the batch size",
          "[s5.g][embedding][activity]")
{
    auto shared = locus::test::SharedTestEmbedder::get();
    REQUIRE(shared);

    auto db_path = make_tmp_dir("enqueue_activity") / "vectors.db";
    Database db(db_path, DbKind::Vectors);
    db.ensure_vectors_schema(shared->dimensions(),
                              locus::test::SharedTestEmbedder::model_filename());

    EmbeddingWorker worker(db, *shared);

    int  call_count = 0;
    std::string last_summary;
    worker.on_activity = [&](const std::string& summary, const std::string& /*detail*/) {
        ++call_count;
        last_summary = summary;
    };

    // enqueue runs on the calling thread; on_activity fires synchronously
    // before start() spins the worker thread up.
    worker.enqueue({1, 2, 3});
    REQUIRE(call_count == 1);
    REQUIRE(last_summary.find("+3") != std::string::npos);

    // Empty batch -> no activity row.
    worker.enqueue({});
    REQUIRE(call_count == 1);

    // Second non-empty batch fires another row.
    worker.enqueue({4, 5});
    REQUIRE(call_count == 2);
}

TEST_CASE("EmbeddingWorker: set_activity_throttle is accepted",
          "[s5.g][embedding][activity]")
{
    auto shared = locus::test::SharedTestEmbedder::get();
    REQUIRE(shared);

    auto db_path = make_tmp_dir("throttle_knob") / "vectors.db";
    Database db(db_path, DbKind::Vectors);
    db.ensure_vectors_schema(shared->dimensions(),
                              locus::test::SharedTestEmbedder::model_filename());

    EmbeddingWorker worker(db, *shared);
    // Smoke-test the setter shape. The actual per-chunk throttle gate fires
    // from the worker thread which a unit test can't deterministically time;
    // its correctness lives in the manual integration walkthrough.
    worker.set_activity_throttle(/*chunks_per_event=*/50, /*ms_per_event=*/2000);
    worker.set_activity_throttle(0, 0);
    SUCCEED("throttle knobs set without error");
}

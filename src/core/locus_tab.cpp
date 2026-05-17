#include "core/locus_tab.h"
#include "core/workspace.h"
#include "../agent/system_prompt.h"
#include "../index/indexer.h"
#include "../tools/process_registry.h"
#include "../tools/process_sink.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace locus {

// -- AutoSaveFrontend --------------------------------------------------------
//
// Listens to the agent's IFrontend events and triggers `tab->persist()` on
// every history mutation. This is the load-bearing implementation of the
// "auto-save on every history mutation" rule from the S5.I stage doc.
//
// The IFrontend pure virtuals are no-ops -- this frontend is a side-channel
// for save, not a UI surface.
//
// Fires on the agent thread (per ConversationOwnerScope), so `tab_->persist()`
// reads `history()` while the owner-thread fence still grants write access.
class LocusTab::AutoSaveFrontend : public IFrontend {
public:
    explicit AutoSaveFrontend(LocusTab* tab) : tab_(tab) {}

    void on_turn_start() override {}
    void on_token(std::string_view) override {}
    void on_tool_call_pending(const ToolCall&, const std::string&,
                              bool, const std::vector<std::string>&) override {}
    void on_tool_result(const std::string&, const std::string&, bool) override {
        try_save();
    }
    void on_turn_complete() override {
        try_save();
    }
    void on_context_meter(int, int, int, int, int) override {}
    void on_compaction_needed(int, int) override {}
    void on_session_reset() override {
        // After reset the tab's history is empty -- no save (lazy creation
        // re-applies on the next user message). The session_id stays put on
        // disk; the user can open the old saved version from the menu.
    }
    void on_error(const std::string&) override {}
    void on_embedding_progress(int, int) override {}
    void on_activity(const ActivityEvent&) override {}

    void on_history_message_added(int /*history_id*/,
                                   MessageRole /*role*/,
                                   bool /*deletable*/) override {
        try_save();
    }
    void on_history_message_deleted(int /*history_id*/) override {
        try_save();
    }

private:
    void try_save() {
        // Skip during load_session's re-announce flow -- the data already came
        // from disk; saving again would just rewrite the same JSON N times and
        // (worse, before the load_session reorder fix) could write under a
        // fresh session_id while session_id_ was still empty.
        if (tab_->is_loading()) return;
        // No-op until the tab has at least one user message (lazy session-file
        // creation rule). Persist runs synchronously on the agent thread; the
        // disk-I/O cost is acceptable for small JSONs and a single workspace
        // user. Falls back to a warn log on failure -- we never want a save
        // hiccup to kill the agent.
        try {
            tab_->persist();
        } catch (const std::exception& ex) {
            spdlog::warn("auto-save failed: {}", ex.what());
        }
    }

    LocusTab* tab_;
};

namespace {

// IWorkspaceServices wrapper that delegates everything to a backing Workspace
// except `processes()` -- the tab's own per-tab ProcessRegistry is returned
// instead, so background processes spawned by tools in this tab die when the
// tab does. `process_sink()` still returns the workspace's shared broker so
// the TerminalPanel sees output from every tab through one fan-in point.
class TabServices : public IWorkspaceServices {
public:
    TabServices(Workspace& ws, ProcessRegistry& procs)
        : ws_(ws), procs_(procs) {}

    const std::filesystem::path& root() const override { return ws_.root(); }
    IndexQuery*        index() override        { return ws_.index(); }
    EmbeddingWorker*   embedder() override     { return ws_.embedder(); }
    Reranker*          reranker() override     { return ws_.reranker(); }
    ProcessRegistry*   processes() override    { return &procs_; }
    ProcessSinkBroker* process_sink() override { return ws_.process_sink(); }
    MemoryStore*       memory() override       { return ws_.memory(); }
    Workspace*         workspace() override    { return &ws_; }

private:
    Workspace&       ws_;
    ProcessRegistry& procs_;
};

} // namespace

LocusTab::LocusTab(int tab_id,
                   Workspace& workspace,
                   ILLMClient& llm,
                   IToolRegistry& tools,
                   const LLMConfig& llm_config,
                   const std::filesystem::path& sessions_dir,
                   const std::filesystem::path& checkpoints_dir,
                   const std::filesystem::path& project_prompts_dir,
                   const std::filesystem::path& global_prompts_dir,
                   ProcessSinkBroker* process_sink_broker,
                   SessionManager& sessions)
    : tab_id_(tab_id),
      title_("New tab"),
      sessions_(sessions),
      sessions_dir_(sessions_dir)
{
    // Per-tab background-process registry. Shares the workspace broker so all
    // tabs' output funnels into the same TerminalPanel.
    processes_ = std::make_unique<ProcessRegistry>(
        workspace.root(),
        static_cast<std::size_t>(workspace.config().process_output_buffer_kb) * 1024,
        process_sink_broker);

    services_ = std::make_unique<TabServices>(workspace, *processes_);

    WorkspaceMetadata ws_meta;
    ws_meta.root          = workspace.root();
    auto& st              = workspace.indexer().stats();
    ws_meta.file_count    = static_cast<int>(st.files_total);
    ws_meta.symbol_count  = static_cast<int>(st.symbols_total);
    ws_meta.heading_count = static_cast<int>(st.headings_total);

    agent_ = std::make_unique<AgentCore>(
        llm, tools, *services_,
        workspace.locus_md(), ws_meta, llm_config,
        sessions_dir, checkpoints_dir,
        project_prompts_dir, global_prompts_dir);

    autosave_fe_ = std::make_unique<AutoSaveFrontend>(this);
    agent_->register_frontend(autosave_fe_.get());

    agent_->start();
}

LocusTab::~LocusTab()
{
    // Agent thread first -- its callbacks may dereference processes_ during
    // a tool call in flight. unique_ptr destruction order then tears down
    // services_ (a thin wrapper, safe to drop) and finally processes_.
    if (agent_) {
        if (autosave_fe_)
            agent_->unregister_frontend(autosave_fe_.get());
        agent_->stop();
    }
}

void LocusTab::set_title(std::string t)
{
    title_ = std::move(t);
    if (!session_id_.empty())
        sessions_.rename(session_id_, title_);
}

bool LocusTab::has_user_message() const
{
    if (!agent_) return false;
    for (const auto& m : agent_->history().messages()) {
        if (m.role == MessageRole::user) return true;
    }
    return false;
}

bool LocusTab::maybe_autoderive_title()
{
    // Only autoderive when the title is still the default "New tab" -- once
    // the user has renamed it we leave it alone.
    if (title_ != "New tab" && !title_.empty()) return false;
    for (const auto& m : agent_->history().messages()) {
        if (m.role != MessageRole::user) continue;
        std::string preview = m.content;
        // Collapse whitespace, trim.
        std::string clean;
        clean.reserve(preview.size());
        for (char c : preview) {
            if (c == '\n' || c == '\r' || c == '\t') {
                if (!clean.empty() && clean.back() != ' ') clean.push_back(' ');
            } else {
                clean.push_back(c);
            }
        }
        // ltrim/rtrim
        auto not_space = [](unsigned char c) { return c != ' '; };
        auto first = std::find_if(clean.begin(), clean.end(), not_space);
        auto last  = std::find_if(clean.rbegin(), clean.rend(), not_space).base();
        if (first >= last) return false;
        clean.assign(first, last);
        if (clean.size() > 30) {
            clean.resize(27);
            clean += "...";
        }
        title_ = clean;
        if (!session_id_.empty())
            sessions_.rename(session_id_, title_);
        return true;
    }
    return false;
}

std::string LocusTab::persist()
{
    if (!has_user_message()) return {};

    maybe_autoderive_title();

    nlohmann::json extras;
    extras["title"] = title_;

    if (session_id_.empty()) {
        session_id_ = sessions_.save(agent_->history(), extras);
    } else {
        sessions_.save(session_id_, agent_->history(), extras);
    }
    return session_id_;
}

std::string LocusTab::persist_as_new()
{
    if (!has_user_message()) return {};
    maybe_autoderive_title();
    nlohmann::json extras;
    extras["title"] = title_;
    session_id_ = sessions_.save(agent_->history(), extras);
    return session_id_;
}

void LocusTab::load_session(const std::string& id)
{
    // Set session_id_ BEFORE the agent load so AutoSaveFrontend (if its
    // is_loading guard ever flips for any reason) doesn't see an empty id
    // and generate a fresh one.  Also bump last-opened first so the restore-
    // bookkeeping picks it up even if the agent load throws.
    session_id_ = id;
    sessions_.bump_last_opened_at(id);
    // Try to pick up the title from the session file.
    for (const auto& info : sessions_.list()) {
        if (info.id == id) {
            if (!info.title.empty()) title_ = info.title;
            break;
        }
    }

    // Suppress AutoSaveFrontend during the re-announce flow inside
    // agent_->load_session -- the data already came from disk.
    loading_ = true;
    try {
        agent_->load_session(id);
    } catch (...) {
        loading_ = false;
        throw;
    }
    loading_ = false;

    if (title_.empty()) maybe_autoderive_title();
}

} // namespace locus

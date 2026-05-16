#pragma once

#include "agent/agent_core.h"
#include "agent/session_manager.h"
#include "core/locus_tab.h"
#include "llm/llm_client.h"
#include "../tools/tool_registry.h"
#include "workspace.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace locus {

class McpManager;

// One bundled lifecycle for everything that hangs off an open workspace:
// `Workspace` -> `ILLMClient` -> `ToolRegistry` -> N x `LocusTab` (each owning
// its own `AgentCore`). Frontends (CLI, GUI, future Crow server) construct one
// of these and forget about wiring order -- destruction unwinds tabs first,
// then shared resources in reverse declaration order automatically.
//
// Threading: every tab's AgentCore runs its own agent thread, lit up at the
// end of `add_tab()` and stopped at the top of the LocusTab destructor.
//
// LLM config resolution layers (lowest to highest priority -- each non-empty
// value overrides the previous):
//   1. defaults baked into LLMConfig
//   2. workspace .locus/config.json   (llm_endpoint / llm_model / ... )
//   3. seed_config passed in by the caller (CLI args, GUI startup args)
//   4. live `query_model_info` from the server (only fills *empty* fields)
class LocusSession {
public:
    // Throws std::runtime_error on failure (Workspace ctor, model probe).
    // Constructs the workspace + LLM + tools but adds NO tabs -- callers
    // must call `add_tab()` (or `restore_tabs(...)`) explicitly. Callers
    // that just want the legacy "one tab" shape can use `add_default_tab()`.
    LocusSession(const std::filesystem::path& ws_path,
                 LLMConfig                    seed_config = {});

    ~LocusSession();

    LocusSession(const LocusSession&)            = delete;
    LocusSession& operator=(const LocusSession&) = delete;

    // -- Shared resources ----------------------------------------------------

    Workspace&     workspace()         { return *workspace_; }
    IToolRegistry& tools()             { return *tools_; }
    ILLMClient&    llm()               { return *llm_; }
    McpManager*    mcp()               { return mcp_.get(); }
    SessionManager& sessions()         { return *sessions_; }
    const LLMConfig& llm_config() const { return llm_config_; }

    // -- Tabs ----------------------------------------------------------------

    // Add a fresh empty tab and return its 0-based index. Lights up the
    // agent thread. The new tab is NOT auto-activated -- the caller decides
    // (the GUI usually calls `set_active_tab(add_tab())`).
    int  add_tab();

    // Add a tab and immediately load `session_id` into it (used by
    // restore-on-workspace-open and by Open from the Saved Sessions menu).
    // Throws on load failure (caller decides whether to fall back to an
    // empty tab).
    int  add_tab_for_session(const std::string& session_id);

    // Close (and discard if empty / move to Saved Sessions if non-empty) the
    // tab at `index`. Empty tabs vanish without disk artifact; non-empty tabs
    // persist their session file before tear-down. If `index` is the last
    // tab, a fresh empty tab is opened in its place so the workspace is
    // never tab-less.
    void close_tab(int index);

    // Close the tab AND remove its on-disk session file (when present).
    void delete_tab(int index);

    // Returns the closed session id (empty for an empty-discarded tab) when a
    // close would happen. Used by callers that want to confirm before doing it.
    std::string peek_close_session_id(int index) const;

    int  tab_count() const                   { return static_cast<int>(tabs_.size()); }
    LocusTab&       tab(int index)            { return *tabs_.at(index); }
    const LocusTab& tab(int index) const      { return *tabs_.at(index); }
    LocusTab&       active_tab()              { return *tabs_.at(active_index_); }
    int  active_index() const                 { return active_index_; }
    void set_active_tab(int index);

    // Find the (first) tab whose session_id matches the given id. Returns -1
    // if none. Used by the menu to filter currently-open sessions out of the
    // Saved Sessions list.
    int find_tab_by_session(const std::string& session_id) const;

    // Set of session ids of currently-open tabs (workspace-wide). Cheap.
    std::unordered_set<std::string> open_session_ids() const;

    // Legacy accessor: returns the active tab's AgentCore. Used by callers
    // (CLI REPL, integration tests, pre-S5.I GUI plumbing) that still see a
    // workspace as a single conversation.
    AgentCore& agent() { return active_tab().agent(); }

    // Persist every non-empty tab to disk and return the set of saved
    // session ids. Used by the GUI when capturing `open_tabs` for the
    // workspace UI state at close.
    std::vector<std::string> persist_all_tabs();

private:
    void load_shared_resources(const std::filesystem::path& ws_path,
                               LLMConfig                    seed_config);

    // -- Shared, in destruction order (workspace last)
    std::unique_ptr<Workspace>     workspace_;
    std::unique_ptr<ILLMClient>    llm_;
    std::unique_ptr<ToolRegistry>  tools_;
    std::unique_ptr<McpManager>    mcp_;
    std::unique_ptr<SessionManager> sessions_;  // workspace-shared session store

    // Tab list. Order matters: tabs MUST be destroyed before shared
    // resources, because each LocusTab's AgentCore holds references to llm_
    // / tools_ / workspace_.
    std::vector<std::unique_ptr<LocusTab>> tabs_;
    int                                    active_index_ = 0;
    int                                    next_tab_id_  = 1;

    LLMConfig             llm_config_;
    std::filesystem::path checkpoints_dir_;
    std::filesystem::path project_prompts_dir_;
    std::filesystem::path global_prompts_dir_;
};

} // namespace locus

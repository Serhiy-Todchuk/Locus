#pragma once

#include "core/workspace_services.h"
#include "../agent/agent_core.h"
#include "../agent/session_manager.h"
#include "../core/frontend.h"
#include "../llm/llm_client.h"
#include "../tools/tool.h"

#include <filesystem>
#include <memory>
#include <string>

namespace locus {

class ProcessRegistry;
class ProcessSinkBroker;
class Workspace;

// S5.I -- one conversation tab inside a workspace. Owns the per-tab
// `AgentCore`, the per-tab `ProcessRegistry`, and the per-tab title/session
// id. References to the workspace-shared substrate (workspace itself, LLM
// client, tool registry, session manager) come from the LocusSession.
//
// Lifetime: constructed in `LocusSession::add_tab`; destructor stops the
// agent thread first (so callbacks don't fire into freed members), then
// tears down the tab's ProcessRegistry which kills any bg processes the tab
// spawned. The tab's session JSON on disk is left untouched -- it's the
// menu's job to delete saved sessions.
class LocusTab {
public:
    LocusTab(int tab_id,
             Workspace& workspace,
             ILLMClient& llm,
             IToolRegistry& tools,
             const LLMConfig& llm_config,
             const std::filesystem::path& sessions_dir,
             const std::filesystem::path& checkpoints_dir,
             const std::filesystem::path& project_prompts_dir,
             const std::filesystem::path& global_prompts_dir,
             ProcessSinkBroker* process_sink_broker,
             SessionManager& sessions);

    ~LocusTab();

    LocusTab(const LocusTab&)            = delete;
    LocusTab& operator=(const LocusTab&) = delete;

    int                tab_id() const                { return tab_id_; }
    const std::string& title() const                 { return title_; }
    void               set_title(std::string t);

    // Empty until the first user message lands on this tab (lazy session-file
    // creation). When non-empty the tab maps 1:1 to a JSON file in the
    // workspace's sessions dir.
    const std::string& session_id() const            { return session_id_; }
    void               set_session_id(std::string id) { session_id_ = std::move(id); }

    AgentCore&         agent()                       { return *agent_; }
    const AgentCore&   agent() const                 { return *agent_; }
    ProcessRegistry&   processes()                   { return *processes_; }

    // True iff the agent has at least one non-system message. Drives the
    // "empty tab is silently discarded" rule on close.
    bool has_user_message() const;

    // Persist the conversation to disk via SessionManager. On first call (no
    // session_id yet), generates an id and writes a fresh file with the tab
    // title; subsequent calls overwrite the same file. No-op on empty tabs.
    // Returns the session id on success (or the existing one on no-op),
    // empty string if nothing was saved.
    std::string persist();

    // Force a fresh save with a new session id (used internally for cloning,
    // not user-facing). Returns the new id.
    std::string persist_as_new();

    // Load a saved session into this tab. Reassigns `session_id_` to `id` and
    // sets the title from the session file. Throws on failure.
    void load_session(const std::string& id);

    // Title autoderived from the first user message in history if the tab has
    // none yet. Returns true if the title was just set this call.
    bool maybe_autoderive_title();

private:
    // S5.I -- IFrontend implementation registered with this tab's AgentCore
    // so every history mutation auto-saves. Lives inside LocusTab because the
    // "save the right tab's history" routing is keyed off the LocusTab
    // instance itself. Defined in the .cpp.
    class AutoSaveFrontend;

    int          tab_id_;
    std::string  title_;
    std::string  session_id_;

    SessionManager& sessions_;
    std::filesystem::path sessions_dir_;

    // Per-tab subsystems. Order matters: AgentCore must die first (joins its
    // thread) before processes_ tears down so a tool call in flight can't
    // dereference a freed registry.
    std::unique_ptr<ProcessRegistry>    processes_;
    std::unique_ptr<IWorkspaceServices> services_;     // TabServices impl in .cpp
    std::unique_ptr<AgentCore>          agent_;
    std::unique_ptr<AutoSaveFrontend>   autosave_fe_;  // registered last
};

} // namespace locus

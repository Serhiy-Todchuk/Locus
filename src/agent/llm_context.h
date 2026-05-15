#pragma once

#include "checkpoint_store.h"
#include "context_budget.h"
#include "conversation.h"
#include "file_change_tracker.h"
#include "session_manager.h"
#include "system_prompt_assembly.h"
#include "core/workspace_services.h"
#include "../core/frontend.h"           // AttachedContext
#include "llm/llm_client.h"             // LLMConfig

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace locus {

class FrontendRegistry;

// LLMContext (S5.J) -- single owner for the conversation's full LLM-facing
// state.
//
// Before S5.J, AgentCore owned fourteen members; nine of them described what
// the LLM sees (history, system prompt, budget, file tracker, attached
// context, checkpoints, session/turn ids, attached mutex) and the other five
// were agent-loop machinery (the loop itself, the dispatcher, slash commands,
// metrics, the agent thread). The boundary was implicit and on the verge of
// god-class again. LLMContext is that boundary made explicit -- AgentCore
// composes one of these as `ctx_` and operates on it instead of orchestrating
// the parts directly.
//
// The class is deliberately a concrete type (no ILLMContext interface). If a
// remote-context surface ever needs polymorphism, introduce it then.
//
// Threading: mutation methods must run on the agent thread when the
// underlying ConversationHistory has an owner thread set (the existing
// `ConversationOwnerScope` RAII gate from S3.I gates the agent-thread path).
// Tests that drive an LLMContext directly without an agent loop run on a
// single thread and never trip the fence.
class LLMContext {
public:
    LLMContext(IWorkspaceServices&                      services,
               const LLMConfig&                         llm_config,
               SystemPromptAssembly                     prompt,
               FrontendRegistry&                        frontends,
               SessionManager&                          sessions,
               const std::filesystem::path&             checkpoints_dir = {});

    ~LLMContext() = default;

    LLMContext(const LLMContext&)            = delete;
    LLMContext& operator=(const LLMContext&) = delete;

    // -- Read surface --------------------------------------------------------

    const SystemPromptAssembly& system_prompt() const { return prompt_; }
    std::size_t system_prompt_hash() const { return prompt_.hash(); }

    ConversationHistory&       history()       { return history_; }
    const ConversationHistory& history() const { return history_; }

    ContextBudget&       budget()       { return *budget_; }
    const ContextBudget& budget() const { return *budget_; }

    FileChangeTracker* change_tracker() { return change_tracker_.get(); }
    CheckpointStore*   checkpoints()    { return checkpoints_.get(); }

    SessionManager&    sessions() { return sessions_; }
    IWorkspaceServices& services() { return services_; }
    const LLMConfig&   llm_config() const { return llm_config_; }

    // Best-effort current token count (server-reported when present, else
    // heuristic).
    int current_tokens() const;

    // Effective context limit. S5.D will subtract `reserve_tokens()` here;
    // pre-S5.D this just returns `llm_config_.context_limit`.
    int effective_limit() const { return llm_config_.context_limit; }

    // S5.D forward-looking surface. Stubbed to 0 / false here so callers can
    // already wire to LLMContext; the real implementation lands with S5.D.
    int  reserve_tokens() const { return 0; }
    bool would_breach_reserve(int /*incoming_estimate*/) const { return false; }

    // -- Session / turn identity (S4.B) -------------------------------------

    const std::string& session_id() const { return session_id_; }
    int                turn_id()    const { return turn_id_; }

    // Bumps the turn counter and returns the new value. Called at the top of
    // each user turn by AgentCore.
    int bump_turn_id() { return ++turn_id_; }

    // Drop the current checkpoint session and roll a fresh session id.
    // Resets turn_id_ to 0. Used by reset_conversation().
    void renew_session_id();

    // -- Attached file context (S2.4 / S4.F) --------------------------------
    //
    // The attached-file block used to live inside the system prompt; S4.F
    // moved it to a per-turn user-message prepend so the system prompt stays
    // byte-stable. LLMContext owns the slot but does NOT broadcast or emit
    // activity events -- AgentCore wraps `set_attached_context` /
    // `clear_attached_context` and fans the change out to frontends. The
    // mutators here are pure state writes.
    void set_attached_context(AttachedContext ctx);
    void clear_attached_context();
    std::optional<AttachedContext> attached_context() const;

    // Compose the per-user-turn prepend (path + outline + preview) for the
    // currently attached file. Returns "" when nothing is attached.
    std::string compose_attached_context_block() const;

    // -- Mutations (owner-thread-only) --------------------------------------
    //
    // These thin wrappers exist so callers can ignore the underlying
    // ConversationHistory shape and so future S5.E / S5.H surface (pin
    // states, edit/delete/rewind) can land here without touching tools.
    void add_message(ChatMessage msg);
    void replace_system_prompt_in_history(std::string content);

    // -- Compaction ----------------------------------------------------------

    void compact_drop_tool_results();
    void compact_drop_oldest_turns(int n);

    // -- Reset ---------------------------------------------------------------
    //
    // Clears history, re-seeds the system message from the assembly, resets
    // the budget, drops the current checkpoint session + rolls a fresh id,
    // and re-snapshots the file-change tracker. The attached-context slot is
    // deliberately preserved (the user's pin isn't owned by a conversation).
    void reset_for_new_conversation();

    // -- Persistence ---------------------------------------------------------

    // Save the current history through the shared SessionManager. `extras`
    // is merged into the top-level JSON (e.g. metrics snapshot from
    // AgentCore). Returns the session id assigned by SessionManager.
    std::string save_session(const nlohmann::json& extras = {});

    // Load a saved session into `history_`. Resets the budget. Does NOT
    // touch session_id_ / turn_id_ (checkpoint identity is separate from
    // saved-session identity by design).
    void load_session(const std::string& id);

    // -- Helpers -------------------------------------------------------------

    // Take an initial file-change-tracker snapshot from the workspace index,
    // if one is available. Called once at construction (we'd otherwise replay
    // every indexed file as "new" on the first user turn) and again after
    // reset / load. Wrapped in try/catch so a failure here can't take the
    // session down.
    void snapshot_change_tracker();

    // S5.C read access: byte-for-byte pre-mutation snapshot for `rel_path`
    // in the current turn's checkpoint, or nullopt if missing / skipped.
    std::optional<std::string> read_current_pre_mutation(
        const std::string& rel_path) const;

private:
    IWorkspaceServices&  services_;
    LLMConfig            llm_config_;
    SystemPromptAssembly prompt_;
    FrontendRegistry&    frontends_;
    SessionManager&      sessions_;

    ConversationHistory                history_;
    std::unique_ptr<ContextBudget>     budget_;
    std::unique_ptr<FileChangeTracker> change_tracker_;
    std::unique_ptr<CheckpointStore>   checkpoints_;

    mutable std::mutex             attached_mutex_;
    std::optional<AttachedContext> attached_context_;

    std::string session_id_;
    int         turn_id_ = 0;
};

} // namespace locus

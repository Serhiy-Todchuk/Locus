#include "core/locus_session.h"

#include "agent/prompt_templates.h"
#include "agent/system_prompt.h"
#include "index/embedding_worker.h"
#include "index/index_query.h"
#include "index/indexer.h"
#include "mcp/mcp_manager.h"
#include "tools/tools.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <utility>

namespace locus {

namespace {

// Layer the workspace's persisted config underneath any seed values the caller
// (CLI args, GUI startup args) supplied. Empty/zero seed fields adopt the
// workspace-config value; non-empty seed fields win -- that's the contract
// described in the header.
void merge_workspace_defaults(LLMConfig& cfg, const WorkspaceConfig& wc)
{
    if (cfg.base_url.empty() && !wc.llm_endpoint.empty())
        cfg.base_url = wc.llm_endpoint;
    if (cfg.model.empty() && !wc.llm_model.empty())
        cfg.model = wc.llm_model;
    if (cfg.context_limit <= 0 && wc.llm_context_limit > 0)
        cfg.context_limit = wc.llm_context_limit;
    if (wc.llm_max_tokens > 0)
        cfg.max_tokens = wc.llm_max_tokens;
    if (wc.llm_timeout_ms > 0)
        cfg.timeout_ms = wc.llm_timeout_ms;
    if (wc.llm_temperature > 0.0 && wc.llm_temperature != cfg.temperature)
        cfg.temperature = wc.llm_temperature;
    if (cfg.tool_format == ToolFormat::Auto && !wc.llm_tool_format.empty())
        cfg.tool_format = tool_format_from_string(wc.llm_tool_format);

    if (wc.llm_top_p          > 0.0) cfg.top_p          = wc.llm_top_p;
    if (wc.llm_top_k          > 0)   cfg.top_k          = wc.llm_top_k;
    if (wc.llm_min_p          > 0.0) cfg.min_p          = wc.llm_min_p;
    if (wc.llm_repeat_penalty > 0.0) cfg.repeat_penalty = wc.llm_repeat_penalty;
    if (wc.llm_frequency_penalty != 0.0) cfg.frequency_penalty = wc.llm_frequency_penalty;
    if (wc.llm_presence_penalty  != 0.0) cfg.presence_penalty  = wc.llm_presence_penalty;
}

} // namespace

LocusSession::LocusSession(const std::filesystem::path& ws_path,
                           LLMConfig                    seed_config)
    : llm_config_(std::move(seed_config))
{
    load_shared_resources(ws_path, llm_config_);
}

void LocusSession::load_shared_resources(const std::filesystem::path& ws_path,
                                        LLMConfig                    /*seed_config*/)
{
    // 1. Workspace.
    workspace_ = std::make_unique<Workspace>(ws_path);

    if (!workspace_->locus_md().empty())
        spdlog::info("LOCUS.md loaded ({} bytes)", workspace_->locus_md().size());

    auto& st = workspace_->indexer().stats();
    spdlog::info("Index: {} files ({} text, {} binary), {} symbols, {} headings",
                 st.files_total, st.files_indexed, st.files_binary,
                 st.symbols_total, st.headings_total);

    // 2. LLM config / client.
    merge_workspace_defaults(llm_config_, workspace_->config());
    if (llm_config_.base_url.empty())
        llm_config_.base_url = "http://127.0.0.1:1234";

    llm_ = create_llm_client(llm_config_);

    auto model_info = llm_->query_model_info();
    if (!model_info.id.empty() && llm_config_.model.empty()) {
        llm_config_.model = model_info.id;
        spdlog::info("Model auto-detected: {}", llm_config_.model);
    }
    if (llm_config_.context_limit <= 0) {
        if (model_info.context_length > 0) {
            llm_config_.context_limit = model_info.context_length;
            spdlog::info("Context limit (from server): {}", llm_config_.context_limit);
        } else {
            llm_config_.context_limit = 8192;
            spdlog::info("Context limit (default): {}", llm_config_.context_limit);
        }
    } else {
        spdlog::info("Context limit: {}", llm_config_.context_limit);
    }

    // 3. Tool registry + MCP.
    tools_ = std::make_unique<ToolRegistry>();
    register_builtin_tools(*tools_);
    spdlog::info("Tools: {} built-in registered", tools_->all().size());

    mcp_ = std::make_unique<McpManager>(*tools_, workspace_->root());
    mcp_->start_all();
    if (mcp_->server_count() > 0)
        spdlog::info("Tools: {} total after MCP registration", tools_->all().size());

    // 4. Session manager (workspace-shared).
    auto sessions_dir = workspace_->locus_dir() / "sessions";
    sessions_         = std::make_unique<SessionManager>(sessions_dir);

    checkpoints_dir_     = workspace_->locus_dir() / "checkpoints";
    project_prompts_dir_ = workspace_->locus_dir() / "prompts";
    global_prompts_dir_  = PromptTemplateRegistry::default_global_dir();
}

LocusSession::~LocusSession()
{
    // Drop callbacks that capture the active tab's agent first -- the embedding
    // worker thread is still running until ~Workspace, and a chunk-completion
    // firing on_activity after a tab's AgentCore is gone would dereference a
    // freed pointer.
    if (workspace_) {
        workspace_->indexer().on_activity = nullptr;
        if (auto* ew = workspace_->embedding_worker())
            ew->on_activity = nullptr;
    }

    // Tabs first -- each tab's destructor stops its agent thread. unique_ptr
    // destruction order then unwinds tools -> mcp -> llm -> workspace.
    tabs_.clear();
}

// -- Tabs --------------------------------------------------------------------

int LocusSession::add_tab()
{
    int tab_id = next_tab_id_++;

    auto tab = std::make_unique<LocusTab>(
        tab_id,
        *workspace_,
        *llm_,
        *tools_,
        llm_config_,
        sessions_->sessions_dir(),
        checkpoints_dir_,
        project_prompts_dir_,
        global_prompts_dir_,
        workspace_->process_sink(),
        *sessions_);

    // Re-wire the workspace's indexer / embedding callbacks onto the new tab's
    // agent so file-watcher activity continues to land in the active surface.
    // We route to the LAST-ADDED tab here so that newly-spawned tabs see fresh
    // activity. The frontends_ broadcast on the activity log fans out from
    // there to every registered IFrontend.
    workspace_->indexer().on_activity =
        [agent = &tab->agent()](const std::string& s, const std::string& d) {
            agent->emit_index_event(s, d);
        };
    if (auto* ew = workspace_->embedding_worker()) {
        ew->on_activity =
            [agent = &tab->agent()](const std::string& s, const std::string& d) {
                agent->emit_index_event(s, d);
            };
    }

    tabs_.push_back(std::move(tab));
    int index = static_cast<int>(tabs_.size()) - 1;
    // Don't change `active_index_` -- the caller drives activation.
    return index;
}

int LocusSession::add_tab_for_session(const std::string& session_id)
{
    int idx = add_tab();
    try {
        tabs_[idx]->load_session(session_id);
    } catch (const std::exception& ex) {
        spdlog::warn("LocusSession: failed to load session '{}' into tab: {}",
                     session_id, ex.what());
        // Leave the tab in place but empty; caller decides whether to close it.
    }
    return idx;
}

void LocusSession::set_active_tab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    active_index_ = index;
    // Re-point the workspace's indexer / embedding activity callbacks at the
    // newly-active tab so its activity log stays primary. (Frontends still
    // receive events from every tab via their own WxFrontend instance; this
    // routing decides who hears the indexer/embedder.)
    auto* agent = &tabs_[index]->agent();
    workspace_->indexer().on_activity =
        [agent](const std::string& s, const std::string& d) {
            agent->emit_index_event(s, d);
        };
    if (auto* ew = workspace_->embedding_worker()) {
        ew->on_activity =
            [agent](const std::string& s, const std::string& d) {
                agent->emit_index_event(s, d);
            };
    }
}

std::string LocusSession::peek_close_session_id(int index) const
{
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return {};
    if (!tabs_[index]->has_user_message()) return {};
    return tabs_[index]->session_id();
}

void LocusSession::close_tab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;

    // Persist non-empty tabs before tear-down so the Saved Sessions submenu
    // sees them. Empty tabs (no user message ever sent) vanish silently --
    // lazy session-file creation means nothing was on disk yet.
    if (tabs_[index]->has_user_message())
        tabs_[index]->persist();

    tabs_.erase(tabs_.begin() + index);

    // Workspace must never be tab-less. If the user just closed the last tab,
    // open a fresh empty one in its place.
    if (tabs_.empty()) {
        add_tab();
        active_index_ = 0;
    } else if (active_index_ >= static_cast<int>(tabs_.size())) {
        active_index_ = static_cast<int>(tabs_.size()) - 1;
    } else if (active_index_ > index) {
        --active_index_;
    }

    // Re-point activity routing to whichever tab is active now.
    set_active_tab(active_index_);
}

void LocusSession::delete_tab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;

    std::string sid = tabs_[index]->session_id();

    tabs_.erase(tabs_.begin() + index);

    if (!sid.empty())
        sessions_->remove(sid);

    if (tabs_.empty()) {
        add_tab();
        active_index_ = 0;
    } else if (active_index_ >= static_cast<int>(tabs_.size())) {
        active_index_ = static_cast<int>(tabs_.size()) - 1;
    } else if (active_index_ > index) {
        --active_index_;
    }

    set_active_tab(active_index_);
}

int LocusSession::find_tab_by_session(const std::string& session_id) const
{
    if (session_id.empty()) return -1;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->session_id() == session_id)
            return static_cast<int>(i);
    }
    return -1;
}

std::unordered_set<std::string> LocusSession::open_session_ids() const
{
    std::unordered_set<std::string> ids;
    for (auto& t : tabs_) {
        if (!t->session_id().empty()) ids.insert(t->session_id());
    }
    return ids;
}

std::vector<std::string> LocusSession::persist_all_tabs()
{
    std::vector<std::string> ids;
    for (auto& t : tabs_) {
        auto id = t->persist();
        if (!id.empty()) ids.push_back(id);
    }
    return ids;
}

} // namespace locus

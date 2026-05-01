#include "core/locus_session.h"

#include "agent/system_prompt.h"
#include "index/index_query.h"
#include "index/indexer.h"
#include "tools/tools.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <utility>

namespace locus {

namespace {

// Layer the workspace's persisted config underneath any seed values the caller
// (CLI args, GUI startup args) supplied. Empty/zero seed fields adopt the
// workspace-config value; non-empty seed fields win — that's the contract
// described in the header.
void merge_workspace_defaults(LLMConfig& cfg, const WorkspaceConfig& wc)
{
    if (cfg.base_url.empty() && !wc.llm_endpoint.empty())
        cfg.base_url = wc.llm_endpoint;
    if (cfg.model.empty() && !wc.llm_model.empty())
        cfg.model = wc.llm_model;
    if (cfg.context_limit <= 0 && wc.llm_context_limit > 0)
        cfg.context_limit = wc.llm_context_limit;
    // Stall watchdog. Same shape as context_limit: workspace value wins
    // when positive; the LLMConfig default (180s) holds otherwise.
    if (wc.llm_timeout_ms > 0)
        cfg.timeout_ms = wc.llm_timeout_ms;
    // Temperature has a non-zero default (0.7) so we can't tell "user set it"
    // from "default". Treat any non-default workspace value as an override.
    if (wc.llm_temperature > 0.0 && wc.llm_temperature != cfg.temperature)
        cfg.temperature = wc.llm_temperature;
    // tool_format: caller seeds keep precedence (today the CLI/GUI never
    // sets it, so the workspace value always wins). Default Auto in
    // LLMConfig is the same as default "auto" in WorkspaceConfig, so we
    // can unconditionally read from the workspace.
    if (cfg.tool_format == ToolFormat::Auto && !wc.llm_tool_format.empty())
        cfg.tool_format = tool_format_from_string(wc.llm_tool_format);
}

} // namespace

LocusSession::LocusSession(const std::filesystem::path& ws_path,
                           LLMConfig                    seed_config)
    : llm_config_(std::move(seed_config))
{
    // 1. Workspace — owns the index, watcher, embedder. Throws on bad path
    //    or when another Locus already holds the workspace lock.
    workspace_ = std::make_unique<Workspace>(ws_path);

    if (!workspace_->locus_md().empty())
        spdlog::info("LOCUS.md loaded ({} bytes)",
                     workspace_->locus_md().size());

    auto& st = workspace_->indexer().stats();
    spdlog::info("Index: {} files ({} text, {} binary), {} symbols, {} headings",
                 st.files_total, st.files_indexed, st.files_binary,
                 st.symbols_total, st.headings_total);

    // 2. Layer workspace config under the caller's seed values, then build
    //    the LLM client.
    merge_workspace_defaults(llm_config_, workspace_->config());
    if (llm_config_.base_url.empty())
        llm_config_.base_url = "http://127.0.0.1:1234";

    llm_ = create_llm_client(llm_config_);

    // 3. Probe the server for model id + context length and fill any gaps.
    //    A missing server is non-fatal — context_limit falls back to 8192.
    auto model_info = llm_->query_model_info();
    if (!model_info.id.empty() && llm_config_.model.empty()) {
        llm_config_.model = model_info.id;
        spdlog::info("Model auto-detected: {}", llm_config_.model);
    }
    if (llm_config_.context_limit <= 0) {
        if (model_info.context_length > 0) {
            llm_config_.context_limit = model_info.context_length;
            spdlog::info("Context limit (from server): {}",
                         llm_config_.context_limit);
        } else {
            llm_config_.context_limit = 8192;
            spdlog::info("Context limit (default): {}",
                         llm_config_.context_limit);
        }
    } else {
        spdlog::info("Context limit: {}", llm_config_.context_limit);
    }

    // 4. Tool registry — built-ins only at this layer; MCP / plugin tools
    //    will register themselves later through the session if M5 lands.
    tools_ = std::make_unique<ToolRegistry>();
    register_builtin_tools(*tools_);
    spdlog::info("Tools: {} registered", tools_->all().size());

    // 5. Agent core. Sessions + checkpoints land in `.locus/`.
    WorkspaceMetadata ws_meta;
    ws_meta.root          = workspace_->root();
    ws_meta.file_count    = static_cast<int>(st.files_total);
    ws_meta.symbol_count  = static_cast<int>(st.symbols_total);
    ws_meta.heading_count = static_cast<int>(st.headings_total);

    auto sessions_dir    = workspace_->locus_dir() / "sessions";
    auto checkpoints_dir = workspace_->locus_dir() / "checkpoints";
    agent_ = std::make_unique<AgentCore>(
        *llm_, *tools_, *workspace_,
        workspace_->locus_md(), ws_meta, llm_config_,
        sessions_dir, checkpoints_dir);

    // Route indexer activity (file watcher batches, embedding progress) into
    // the agent's event bus so frontends see it on the same channel as
    // tool calls and LLM tokens.
    workspace_->indexer().on_activity =
        [agent = agent_.get()](const std::string& s, const std::string& d) {
            agent->emit_index_event(s, d);
        };

    // 6. Light the agent thread up. Frontends can register before or after
    //    this call — `register_frontend` is thread-safe.
    agent_->start();
}

LocusSession::~LocusSession()
{
    // Stop the agent thread before any subsystem it touches goes away.
    // unique_ptr destruction order then unwinds: agent → tools → llm →
    // workspace, mirroring the construction sequence.
    if (agent_)
        agent_->stop();
}

} // namespace locus

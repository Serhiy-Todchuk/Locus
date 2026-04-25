#pragma once

#include "core/workspace_services.h"
#include "tool.h"  // for ToolApprovalPolicy

#include <filesystem>
#include <functional>
#include <string>
#include <memory>
#include <unordered_map>

namespace locus {

namespace fs = std::filesystem;

class Database;
class Embedder;
class EmbeddingWorker;
class ExtractorRegistry;
class FileWatcher;
class IndexQuery;
class Indexer;
class ProcessRegistry;
class Reranker;
class WatcherPump;
class WorkspaceLock;

struct WorkspaceConfig {
    // Index settings
    std::vector<std::string> exclude_patterns = {
        "node_modules/**", ".git/**", "*.lock", "dist/**",
        "build/**", ".locus/**", ".vs/**"
    };
    int max_file_size_kb = 1024;
    bool code_parsing_enabled = true;

    // Semantic search (on by default - gracefully disabled if model missing).
    // Embedding dimension is derived from the loaded GGUF, never from config:
    // swapping models triggers an automatic re-embed on next workspace open.
    bool semantic_search_enabled = true;
    std::string embedding_model = "bge-m3-Q8_0.gguf";
    int chunk_size_lines = 80;
    int chunk_overlap_lines = 10;

    // Reranker (cross-encoder, S4.J). Off by default until the model is
    // present so first-run users without the GGUF aren't blocked. When on,
    // search_semantic / search_hybrid fetch reranker_top_k bi-encoder
    // candidates and rerank to the requested max_results.
    bool reranker_enabled = false;
    std::string reranker_model = "bge-reranker-v2-m3-Q8_0.gguf";
    // 20 candidates × ~100ms/rerank in Release on CPU = ~2s wall-clock per
    // search call, which is the practical ceiling before a synchronous
    // tool invocation feels broken. Bump only if you're on a workstation
    // CPU or move to GPU.
    int reranker_top_k = 20;

    // LLM settings (persisted per-workspace)
    std::string llm_endpoint = "http://127.0.0.1:1234";
    std::string llm_model;           // empty = server default
    double      llm_temperature = 0.7;
    int         llm_context_limit = 0; // 0 = auto-detect from server

    // Per-workspace tool approval overrides: tool_name -> policy.
    // Absent entries fall back to the tool's default (ITool::approval_policy()).
    std::unordered_map<std::string, ToolApprovalPolicy> tool_approval_policies;

    // S3.L — token-cost guardrail. AgentCore logs the per-turn tool-manifest
    // size at info level every turn; emits a warning when the manifest crosses
    // this threshold. 4000 ≈ 12% of a 32K context — a reasonable "we've grown
    // too much" signal once M4 adds ~20 more tools.
    int tool_manifest_warn_tokens = 4000;

    // S4.I — per-background-process output ring buffer cap. The reader thread
    // appends stdout+stderr until this many bytes are buffered; older bytes
    // are dropped from the front (the LLM is told how many it missed). 256 KB
    // covers a verbose dev-server log between turns without paging the agent
    // thread, and is still cheap (a few processes × 256 KB is negligible).
    int process_output_buffer_kb = 256;

    // S4.T — between-turn external file-change awareness. When true, AgentCore
    // snapshots indexed file mtimes at end of each assistant turn; on the next
    // user turn it computes the diff (excluding files the agent itself touched
    // that turn) and prepends a one-line note to the user message so the LLM
    // knows which files changed under it. Default on -- the cost is one cheap
    // SQLite scan per turn and a single line in the prompt.
    bool notify_external_changes = true;
};

// Owns all workspace-level resources: config, database, file watcher, LOCUS.md.
// One Workspace instance per open folder. Implements `IWorkspaceServices` so it
// can be passed directly to `AgentCore` and tools — no adapter wrapper needed.
class Workspace : public IWorkspaceServices {
public:
    // Opens a workspace at the given path. Creates .locus/ if absent.
    // Throws std::runtime_error on failure.
    explicit Workspace(const fs::path& root);
    ~Workspace() override;

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // -- IWorkspaceServices ---------------------------------------------------
    const fs::path&  root() const override { return root_; }
    IndexQuery*      index() override      { return query_.get(); }
    EmbeddingWorker* embedder() override   { return embedding_worker_.get(); }
    Reranker*        reranker() override   { return reranker_.get(); }
    ProcessRegistry* processes() override  { return processes_.get(); }
    Workspace*       workspace() override  { return this; }

    // -- Workspace-specific ---------------------------------------------------
    const fs::path& locus_dir() const { return locus_dir_; }

    const WorkspaceConfig& config() const { return config_; }
    WorkspaceConfig& config() { return config_; }
    void save_config();

    // LOCUS.md content (empty string if file doesn't exist). Read-only.
    const std::string& locus_md() const { return locus_md_; }

    Database& database() { return *main_db_; }
    // Semantic DB (chunks + chunk_vectors). Null when semantic search disabled.
    Database* vectors_database() { return vectors_db_.get(); }
    ExtractorRegistry& extractors() { return *extractors_; }
    FileWatcher& file_watcher() { return *watcher_; }
    Indexer& indexer() { return *indexer_; }
    IndexQuery& query() { return *query_; }
    WatcherPump& watcher_pump() { return *watcher_pump_; }

    EmbeddingWorker* embedding_worker() { return embedding_worker_.get(); }

    // Hot-toggle semantic search at runtime (creates/destroys embedder + worker).
    // Returns true on success, false if model not found or init failed.
    bool enable_semantic_search();
    void disable_semantic_search();

    // Process-wide override hook. When set, enable_semantic_search() takes the
    // returned embedder instead of loading a GGUF from disk. The model id
    // recorded in vectors.db comes from the returned embedder's filename, so
    // schema migration sees a stable identity. Designed for tests that share
    // a single small embedder across many Workspace instances. Pass nullptr
    // to revert to the default disk-load path.
    using EmbedderProvider = std::function<std::shared_ptr<Embedder>()>;
    static void set_embedder_provider(EmbedderProvider p);

private:
    void load_config();
    void load_locus_md();

    fs::path root_;
    fs::path locus_dir_;
    WorkspaceConfig config_;
    std::string locus_md_;
    // Held first, released last — stops another Locus process from opening
    // this workspace (or a nested/ancestor one) while we own it.
    std::unique_ptr<WorkspaceLock> lock_;
    std::unique_ptr<Database> main_db_;
    std::unique_ptr<Database> vectors_db_;  // null when semantic disabled
    std::unique_ptr<ExtractorRegistry> extractors_;
    std::unique_ptr<FileWatcher> watcher_;
    std::unique_ptr<Indexer> indexer_;
    std::unique_ptr<IndexQuery> query_;
    // shared_ptr so a process-wide hook (`set_embedder_provider`) can hand the
    // same Embedder to multiple Workspace instances — primarily a test-suite
    // optimisation; production unique-Workspace use is unaffected.
    std::shared_ptr<Embedder> embedder_;
    std::unique_ptr<EmbeddingWorker> embedding_worker_;
    std::unique_ptr<Reranker> reranker_;
    // Owned after `indexer_` so it stops + joins before the indexer is torn
    // down (the pump's background thread feeds events into the indexer).
    std::unique_ptr<WatcherPump> watcher_pump_;
    // S4.I — long-running shell processes spawned by the agent. The dtor
    // terminates every still-running child so workspace close never leaks
    // background processes.
    std::unique_ptr<ProcessRegistry> processes_;
};

} // namespace locus

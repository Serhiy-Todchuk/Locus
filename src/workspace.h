#pragma once

#include "core/workspace_services.h"
#include "tool.h"  // for ToolApprovalPolicy

#include <filesystem>
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
class WatcherPump;

struct WorkspaceConfig {
    // Index settings
    std::vector<std::string> exclude_patterns = {
        "node_modules/**", ".git/**", "*.lock", "dist/**",
        "build/**", ".locus/**", ".vs/**"
    };
    int max_file_size_kb = 1024;
    bool code_parsing_enabled = true;

    // Semantic search (on by default — gracefully disabled if model missing)
    bool semantic_search_enabled = true;
    std::string embedding_model = "all-MiniLM-L6-v2.Q8_0.gguf";
    int embedding_dimensions = 384;
    int chunk_size_lines = 80;
    int chunk_overlap_lines = 10;

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

private:
    void load_config();
    void load_locus_md();

    fs::path root_;
    fs::path locus_dir_;
    WorkspaceConfig config_;
    std::string locus_md_;
    std::unique_ptr<Database> main_db_;
    std::unique_ptr<Database> vectors_db_;  // null when semantic disabled
    std::unique_ptr<ExtractorRegistry> extractors_;
    std::unique_ptr<FileWatcher> watcher_;
    std::unique_ptr<Indexer> indexer_;
    std::unique_ptr<IndexQuery> query_;
    std::unique_ptr<Embedder> embedder_;
    std::unique_ptr<EmbeddingWorker> embedding_worker_;
    // Owned after `indexer_` so it stops + joins before the indexer is torn
    // down (the pump's background thread feeds events into the indexer).
    std::unique_ptr<WatcherPump> watcher_pump_;
};

} // namespace locus

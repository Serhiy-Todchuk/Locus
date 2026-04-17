#pragma once

#include <filesystem>
#include <string>
#include <memory>

namespace locus {

namespace fs = std::filesystem;

class Database;
class Embedder;
class EmbeddingWorker;
class ExtractorRegistry;
class FileWatcher;
class IndexQuery;
class Indexer;

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
    std::string embedding_model = "all-MiniLM-L6-v2.onnx";
    int embedding_dimensions = 384;
    int chunk_size_lines = 80;
    int chunk_overlap_lines = 10;

    // LLM settings (persisted per-workspace)
    std::string llm_endpoint = "http://127.0.0.1:1234";
    std::string llm_model;           // empty = server default
    double      llm_temperature = 0.7;
    int         llm_context_limit = 0; // 0 = auto-detect from server
};

// Owns all workspace-level resources: config, database, file watcher, LOCUS.md.
// One Workspace instance per open folder.
class Workspace {
public:
    // Opens a workspace at the given path. Creates .locus/ if absent.
    // Throws std::runtime_error on failure.
    explicit Workspace(const fs::path& root);
    ~Workspace();

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    const fs::path& root() const { return root_; }
    const fs::path& locus_dir() const { return locus_dir_; }

    const WorkspaceConfig& config() const { return config_; }
    WorkspaceConfig& config() { return config_; }
    void save_config();

    // LOCUS.md content (empty string if file doesn't exist). Read-only.
    const std::string& locus_md() const { return locus_md_; }

    Database& database() { return *db_; }
    ExtractorRegistry& extractors() { return *extractors_; }
    FileWatcher& file_watcher() { return *watcher_; }
    Indexer& indexer() { return *indexer_; }
    IndexQuery& query() { return *query_; }

    // Semantic search (may be null if disabled)
    Embedder* embedder() { return embedder_.get(); }
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
    std::unique_ptr<Database> db_;
    std::unique_ptr<ExtractorRegistry> extractors_;
    std::unique_ptr<FileWatcher> watcher_;
    std::unique_ptr<Indexer> indexer_;
    std::unique_ptr<IndexQuery> query_;
    std::unique_ptr<Embedder> embedder_;
    std::unique_ptr<EmbeddingWorker> embedding_worker_;
};

} // namespace locus

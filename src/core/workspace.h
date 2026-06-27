#pragma once

#include "core/workspace_config.h"
#include "core/workspace_services.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace locus {

namespace fs = std::filesystem;

namespace zim { class ZimArchive; }

class Database;
class Embedder;
class EmbeddingWorker;
class ExtractorRegistry;
class FileWatcher;
class IndexQuery;
class Indexer;
class MemoryStore;
class ProcessRegistry;
class Reranker;
class WatcherPump;
class WorkspaceLock;

// Owns all workspace-level resources: config, database, file watcher, LOCUS.md.
// One Workspace instance per open folder. Implements `IWorkspaceServices` so it
// can be passed directly to `AgentCore` and tools -- no adapter wrapper needed.
class Workspace : public IWorkspaceServices {
public:
    // Opens a workspace at the given path. Creates .locus/ if absent.
    // Throws std::runtime_error on failure.
    //
    // S6.2 -- when `root` points at a `.zim` file (Kiwix / Wikipedia), the
    // workspace opens in READ-ONLY ZIM mode: articles are virtual "files"
    // pulled from the archive and bulk-fed into the index (no file watcher, no
    // filesystem traversal). The index DB lives in a `.locus` dir derived from
    // the archive name, alongside the .zim. read_file serves stripped article
    // text via the index; search surfaces `[wikipedia, untrusted]` snippets.
    explicit Workspace(const fs::path& root);
    ~Workspace() override;

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // -- IWorkspaceServices ---------------------------------------------------
    const fs::path&    root() const override     { return root_; }
    IndexQuery*        index() override          { return query_.get(); }
    EmbeddingWorker*   embedder() override       { return embedding_worker_.get(); }
    Reranker*          reranker() override       { return reranker_.get(); }
    ProcessRegistry*   processes() override      { return processes_.get(); }
    ProcessSinkBroker* process_sink() override;
    MemoryStore*       memory() override         { return memory_.get(); }
    Workspace*         workspace() override      { return this; }

    // -- Workspace-specific ---------------------------------------------------
    const fs::path& locus_dir() const { return locus_dir_; }

    // S6.2 -- true when this workspace is a read-only ZIM archive view. Tools /
    // UI can use it to suppress mutating affordances. The open ZimArchive is
    // kept alive so read_file can pull article HTML on demand (it is the source
    // of truth for content; the index stores stripped text for search).
    bool zim_mode() const { return zim_archive_ != nullptr; }
    zim::ZimArchive* zim_archive() { return zim_archive_.get(); }

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
    // S6.2 -- the ZIM-mode ctor body: opens the archive, builds the index DB,
    // bulk-feeds every article through Indexer::index_virtual_article. Called
    // from the ctor when `root` is a `.zim` file. `progress` (optional) is
    // forwarded the (done,total) article counts so the GUI shows build status.
    void build_zim_index();

    fs::path root_;
    fs::path locus_dir_;
    // S6.2 -- non-null in ZIM mode; kept open for on-demand article reads.
    std::unique_ptr<zim::ZimArchive> zim_archive_;
    WorkspaceConfig config_;
    std::string locus_md_;
    // Held first, released last -- stops another Locus process from opening
    // this workspace (or a nested/ancestor one) while we own it.
    std::unique_ptr<WorkspaceLock> lock_;
    std::unique_ptr<Database> main_db_;
    std::unique_ptr<Database> vectors_db_;  // null when semantic disabled
    std::unique_ptr<ExtractorRegistry> extractors_;
    std::unique_ptr<FileWatcher> watcher_;
    std::unique_ptr<Indexer> indexer_;
    std::unique_ptr<IndexQuery> query_;
    // shared_ptr so a process-wide hook (`set_embedder_provider`) can hand the
    // same Embedder to multiple Workspace instances -- primarily a test-suite
    // optimisation; production unique-Workspace use is unaffected.
    std::shared_ptr<Embedder> embedder_;
    std::unique_ptr<EmbeddingWorker> embedding_worker_;
    std::unique_ptr<Reranker> reranker_;
    // Owned after `indexer_` so it stops + joins before the indexer is torn
    // down (the pump's background thread feeds events into the indexer).
    std::unique_ptr<WatcherPump> watcher_pump_;
    // S4.I -- long-running shell processes spawned by the agent. The dtor
    // terminates every still-running child so workspace close never leaks
    // background processes.
    std::unique_ptr<ProcessRegistry> processes_;
    // S4.R -- workspace-scoped memory bank. Optional: null when
    // `WorkspaceConfig::Memory::enabled` is false. Reuses the workspace
    // embedder and reranker via raw pointers stored at construction time;
    // hot-toggling those isn't supported (re-open the workspace).
    std::unique_ptr<MemoryStore> memory_;
};

} // namespace locus

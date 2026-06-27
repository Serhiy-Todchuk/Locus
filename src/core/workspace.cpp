#include "workspace.h"
#include "core/global_config.h"
#include "core/global_paths.h"
#include "core/memory_store.h"
#include "core/watcher_pump.h"
#include "core/workspace_config_json.h"
#include "core/workspace_lock.h"
#include "database.h"
#include "../index/embedder.h"
#include "../index/embedding_worker.h"
#include "extractors/docx_extractor.h"
#include "extractors/extractor_registry.h"
#include "extractors/html_extractor.h"
#include "extractors/json_extractor.h"
#include "extractors/markdown_extractor.h"
#include "extractors/pdf_extractor.h"
#include "extractors/xlsx_extractor.h"
#include "extractors/yaml_extractor.h"
#include "file_watcher.h"
#include "index/index_query.h"
#include "index/indexer.h"
#include "../tools/process_registry.h"
#include "../index/reranker.h"
#include "security/injection_policy.h"
#include "security/injection_scanner.h"
#include "zim/zim_archive.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace locus {

using json = nlohmann::json;

namespace {
// Process-wide override for embedder construction (S4.B-followup test speedup).
// Guarded by a mutex because tests may set/clear it from a fixture thread while
// other tests are mid-Workspace-construction in the same process.
std::mutex                  g_embedder_provider_mu;
Workspace::EmbedderProvider g_embedder_provider;
} // namespace

void Workspace::set_embedder_provider(EmbedderProvider p)
{
    std::lock_guard lock(g_embedder_provider_mu);
    g_embedder_provider = std::move(p);
}

Workspace::Workspace(const fs::path& root)
{
    fs::path in = fs::absolute(root);
    std::error_code ec;

    // S6.2 -- ZIM mode: the path is a .zim archive, not a directory. The
    // workspace root becomes the archive's parent dir (a real dir we can put
    // .locus in) and the index DB lives in a per-archive subdir so two ZIMs in
    // the same folder don't share an index.
    bool zim = fs::is_regular_file(in, ec) &&
               in.extension() == ".zim";

    if (zim) {
        root_ = in.parent_path();
        // <parent>/.locus-zim-<archive-stem>/ keeps the index next to the .zim
        // and distinct from any real-folder workspace rooted at the parent.
        locus_dir_ = root_ / (".locus-zim-" + in.stem().string());
    } else {
        root_ = in;
        if (!fs::is_directory(root_)) {
            throw std::runtime_error("Not a directory: " + root_.string());
        }
        locus_dir_ = root_ / ".locus";
    }

    fs::create_directories(locus_dir_, ec);
    if (ec) {
        throw std::runtime_error("Cannot create .locus/: " + ec.message());
    }

    // Grab the workspace lock before touching any stateful subsystem -- if
    // another Locus instance owns this folder (or an ancestor of it), we
    // must fail cleanly before opening databases or starting threads. In ZIM
    // mode the lock is scoped to the per-archive .locus dir so two different
    // ZIMs under the same parent can be open at once.
    lock_ = std::make_unique<WorkspaceLock>(zim ? locus_dir_ : root_);

    load_config();
    load_locus_md();

    if (zim) {
        // ZIM mode forces read-only retrieval-only behaviour: no semantic
        // embedding (multi-GB), no memory bank, no file watcher.
        config_.index.semantic_search_enabled = false;
        config_.memory.enabled = false;
        // Open the archive up front so a bad file fails the ctor cleanly.
        zim_archive_ = std::make_unique<zim::ZimArchive>(in);
    }

    spdlog::info("Opening main database at {}", (locus_dir_ / "index.db").string());
    main_db_ = std::make_unique<Database>(locus_dir_ / "index.db", DbKind::Main);
    // One-time migration from the pre-split layout.
    main_db_->drop_legacy_semantic_tables();

    // ZIM mode has no live filesystem to watch -- the archive is immutable.
    if (!zim) {
        spdlog::info("Starting file watcher on {}", root_.string());
        watcher_ = std::make_unique<FileWatcher>(root_, config_.index.exclude_patterns);
        watcher_->start();
    }

    // Register text extractors for known document formats
    extractors_ = std::make_unique<ExtractorRegistry>();
    extractors_->register_extractor(".md",   std::make_unique<MarkdownExtractor>());
    extractors_->register_extractor(".markdown", std::make_unique<MarkdownExtractor>());
    extractors_->register_extractor(".html", std::make_unique<HtmlExtractor>());
    extractors_->register_extractor(".htm",  std::make_unique<HtmlExtractor>());
    extractors_->register_extractor(".pdf",  std::make_unique<PdfiumExtractor>());
    extractors_->register_extractor(".docx", std::make_unique<DocxExtractor>());
    extractors_->register_extractor(".xlsx", std::make_unique<XlsxExtractor>());
    extractors_->register_extractor(".json",  std::make_unique<JsonExtractor>());
    extractors_->register_extractor(".jsonc", std::make_unique<JsonExtractor>());
    extractors_->register_extractor(".yaml",  std::make_unique<YamlExtractor>());
    extractors_->register_extractor(".yml",   std::make_unique<YamlExtractor>());

    // Initialise semantic search if enabled (opens vectors.db on success)
    if (config_.index.semantic_search_enabled) {
        enable_semantic_search();
    }

    spdlog::info("Building workspace index");
    indexer_ = std::make_unique<Indexer>(*main_db_, vectors_db_.get(),
                                         root_, config_, *extractors_);

    // Wire chunking callback to embedding worker
    if (embedding_worker_) {
        indexer_->on_chunks_created = [this](std::vector<int64_t> ids) {
            embedding_worker_->enqueue(std::move(ids));
        };
    }

    if (zim) {
        build_zim_index();
    } else {
        indexer_->build_initial();
    }

    // Start embedding worker after initial index (chunks are queued)
    if (embedding_worker_) {
        embedding_worker_->start();
    }

    query_ = std::make_unique<IndexQuery>(*main_db_, vectors_db_.get());

    // Watcher pump: drains the FileWatcher on a background thread and
    // batches events into Indexer::process_events. Replaces the per-frontend
    // pumps that used to live in main.cpp / locus_app.cpp / locus_frame.cpp.
    // Skipped in ZIM mode (no watcher).
    if (watcher_) {
        watcher_pump_ = std::make_unique<WatcherPump>(*watcher_, *indexer_);
        watcher_pump_->start();
    }

    // Background-process registry (S4.I). Empty at startup; tools spawn into it.
    processes_ = std::make_unique<ProcessRegistry>(
        root_, static_cast<std::size_t>(config_.agent.process_output_buffer_kb) * 1024);

    // S4.R memory bank. Constructed last so it picks up the embedder/reranker
    // that enable_semantic_search wired above. Lives in `.locus/memory/`.
    if (config_.memory.enabled) {
        memory_ = std::make_unique<MemoryStore>(
            locus_dir_ / "memory",
            *main_db_,
            vectors_db_.get(),
            embedding_worker_.get(),
            reranker_.get(),
            config_);
    }

    spdlog::info("Workspace opened: {}", root_.string());
}

Workspace::~Workspace()
{
    // Terminate any background processes the agent left running before the
    // subsystems they might be talking to (file watcher, indexer) go away.
    processes_.reset();

    // Stop the pump first so no late tick fires into a half-destructed
    // indexer. The pump's stop() also performs a final synchronous flush so
    // edits made right before close still get indexed.
    if (watcher_pump_) {
        watcher_pump_->stop();
        watcher_pump_.reset();
    }
    if (embedding_worker_) {
        embedding_worker_->stop();
    }
    if (watcher_) {
        watcher_->stop();
    }
    spdlog::info("Workspace closed: {}", root_.string());
}

void Workspace::build_zim_index()
{
    if (!zim_archive_) return;

    const auto& arts = zim_archive_->articles();
    const size_t total = arts.size();
    spdlog::info("ZIM index build: {} article(s) to feed", total);

    const bool scan = config_.security.scan_zim;
    security::ScannerConfig scfg;
    scfg.max_scan_bytes = static_cast<std::size_t>(config_.security.max_scan_kb) * 1024;

    // Bulk insert under a single transaction -- thousands of upsert/FTS writes
    // per ZIM, and an autocommit-per-row would make a 5000-article build crawl.
    main_db_->exec("BEGIN TRANSACTION");
    size_t done = 0, indexed = 0;
    for (const auto& a : arts) {
        auto html = zim_archive_->read_content(a.entry_index);
        ++done;
        if (!html || html->empty()) {
            if (indexer_ && indexer_->on_progress && (done % 500 == 0 || done == total))
                indexer_->on_progress(static_cast<int>(done), static_cast<int>(total));
            continue;
        }

        // Strip article HTML -> plain text + headings (in-memory, no temp file).
        ExtractionResult ext = HtmlExtractor::extract_from_string(*html);
        if (ext.is_binary || ext.text.empty()) continue;

        // S6.0 Task D -- the origin stamp ("zim") is UNCONDITIONAL; the keyword
        // scan is opt-in (security.scan_zim, default off) because encyclopedia
        // content is overwhelmingly benign and a multi-GB scan isn't worth it.
        uint32_t flags = 0;
        if (scan) {
            security::ScanResult sr = security::scan_for_injection(ext.text, scfg);
            flags = sr.flags_bitmask();
        }

        // Use the title as the virtual path when present (human-readable in
        // list_directory / read_file); fall back to the archive path.
        std::string vpath = a.path.empty() ? a.title : a.path;

        indexer_->index_virtual_article(
            vpath, ext.text, ext.headings, "zim", flags,
            static_cast<int64_t>(html->size()));
        ++indexed;

        if (indexer_ && indexer_->on_progress && (done % 500 == 0 || done == total))
            indexer_->on_progress(static_cast<int>(done), static_cast<int>(total));
    }
    main_db_->exec("COMMIT");

    spdlog::info("ZIM index build complete: {}/{} article(s) indexed",
                 indexed, total);
    if (indexer_ && indexer_->on_activity) {
        indexer_->on_activity(
            "ZIM index built: " + std::to_string(indexed) + " article(s)",
            "Source: " + zim_archive_->path().filename().string());
    }
}

ProcessSinkBroker* Workspace::process_sink()
{
    return processes_ ? processes_->sink_broker() : nullptr;
}

// -- Semantic search hot-toggle -----------------------------------------------

bool Workspace::enable_semantic_search()
{
    if (embedder_ && embedding_worker_)
        return true;  // already active

    // Test-only fast path: if a process-wide provider is registered, take its
    // (already-loaded) embedder rather than mmap+initialising a fresh GGUF.
    EmbedderProvider provider;
    {
        std::lock_guard lock(g_embedder_provider_mu);
        provider = g_embedder_provider;
    }
    if (provider) {
        try {
            embedder_ = provider();
        } catch (const std::exception& e) {
            spdlog::error("EmbedderProvider threw: {}", e.what());
            return false;
        }
        if (!embedder_) {
            spdlog::warn("EmbedderProvider returned null; semantic search disabled");
            return false;
        }
        const int dim = embedder_->dimensions();
        std::string model_id = embedder_->model_path().filename().string();

        if (!vectors_db_) {
            auto vpath = locus_dir_ / "vectors.db";
            spdlog::info("Opening vectors database at {}", vpath.string());
            vectors_db_ = std::make_unique<Database>(vpath, DbKind::Vectors);
        }
        vectors_db_->ensure_vectors_schema(dim, model_id);

        embedding_worker_ = std::make_unique<EmbeddingWorker>(
            *vectors_db_, *embedder_);
        if (indexer_) {
            indexer_->on_chunks_created = [this](std::vector<int64_t> ids) {
                embedding_worker_->enqueue(std::move(ids));
            };
        }
        embedding_worker_->start();
        spdlog::info("Semantic search enabled via shared embedder "
                     "(model: {}, dim: {})", model_id, dim);
        return true;
    }

    // Find models/<name> by walking up from the exe dir.  Supports both packaged
    // layout (models/ next to exe) and dev layout (exe in build/<cfg>/<cfg>/,
    // models/ at repo root -- up to 4 levels up).
    fs::path exe_dir = fs::current_path();
#ifdef _WIN32
    {
        wchar_t buf[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, buf, MAX_PATH))
            exe_dir = fs::path(buf).parent_path();
    }
#endif

    fs::path model_path;
    fs::path base = exe_dir;
    for (int i = 0; i < 6; ++i) {
        fs::path candidate = base / "models" / config_.index.embedding_model;
        if (fs::exists(candidate)) {
            model_path = candidate;
            break;
        }
        if (!base.has_parent_path() || base.parent_path() == base) break;
        base = base.parent_path();
    }

    if (model_path.empty()) {
        spdlog::warn("Semantic search enabled but model '{}' not found under {} or its parents",
                     config_.index.embedding_model, exe_dir.string());
        return false;
    }

    try {
        // Embedder first - we need its native dimension before opening
        // (or migrating) the vectors schema.
        embedder_ = std::make_shared<Embedder>(model_path);
        const int dim = embedder_->dimensions();

        if (!vectors_db_) {
            auto vpath = locus_dir_ / "vectors.db";
            spdlog::info("Opening vectors database at {}", vpath.string());
            vectors_db_ = std::make_unique<Database>(vpath, DbKind::Vectors);
        }

        // If a previous run wrote vectors with a different dim OR a
        // different embedder model (same dim, different vector space),
        // ensure_vectors_schema drops chunk_vectors + chunks; the indexer
        // pass that follows will re-chunk + re-enqueue every file.
        vectors_db_->ensure_vectors_schema(dim, config_.index.embedding_model);

        embedding_worker_ = std::make_unique<EmbeddingWorker>(
            *vectors_db_, *embedder_);

        // Wire indexer callback
        if (indexer_) {
            indexer_->on_chunks_created = [this](std::vector<int64_t> ids) {
                embedding_worker_->enqueue(std::move(ids));
            };
        }

        embedding_worker_->start();
        spdlog::info("Semantic search enabled (model: {}, dim: {})",
                     model_path.string(), dim);

        // Reranker is optional - only load if enabled and the GGUF is present.
        if (config_.index.reranker_enabled) {
            fs::path rr_path;
            fs::path b = exe_dir;
            for (int i = 0; i < 6; ++i) {
                fs::path c = b / "models" / config_.index.reranker_model;
                if (fs::exists(c)) { rr_path = c; break; }
                if (!b.has_parent_path() || b.parent_path() == b) break;
                b = b.parent_path();
            }
            if (rr_path.empty()) {
                spdlog::warn("Reranker enabled but model '{}' not found - "
                             "search will use bi-encoder scores only",
                             config_.index.reranker_model);
            } else {
                try {
                    reranker_ = std::make_unique<Reranker>(rr_path);
                    spdlog::info("Reranker enabled (model: {})", rr_path.string());
                } catch (const std::exception& e) {
                    spdlog::error("Failed to initialise reranker: {} - "
                                  "search will use bi-encoder scores only", e.what());
                    reranker_.reset();
                }
            }
        }
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialise embedder: {}", e.what());
        embedder_.reset();
        embedding_worker_.reset();
        reranker_.reset();
        vectors_db_.reset();
        return false;
    }
}

void Workspace::disable_semantic_search()
{
    if (embedding_worker_) {
        embedding_worker_->stop();
        embedding_worker_.reset();
    }
    embedder_.reset();
    reranker_.reset();
    // Keep vectors_db_ open so existing embeddings remain queryable even while
    // no new ones are being produced.  Callers that want to reclaim disk can
    // close + delete the file separately.

    if (indexer_)
        indexer_->on_chunks_created = nullptr;

    spdlog::info("Semantic search disabled");
}

// -- Config -------------------------------------------------------------------

void Workspace::load_config()
{
    auto path = locus_dir_ / "config.json";

    if (fs::exists(path)) {
        std::ifstream f(path);
        if (f.is_open()) {
            try {
                json j = json::parse(f);
                config_ = workspace_config_from_json(j);
                spdlog::info("Loaded config from {}", path.string());
                return;
            } catch (const json::exception& e) {
                spdlog::warn("Failed to parse config.json, using defaults: {}", e.what());
            }
        }
    }

    // S5.M -- first run for this workspace. Seed from the global template
    // (~/.locus/config.json) when present, falling back to compiled defaults.
    // The seeded values are immediately written back to the workspace config
    // so this code path runs at most once per workspace and subsequent opens
    // read the local file deterministically (no live override semantics).
    config_ = load_global_config_or_defaults();
    save_config();
    if (fs::exists(global_paths::config_path())) {
        spdlog::info("Seeded new workspace config at {} from global defaults",
                     path.string());
    } else {
        spdlog::info("Created default config at {}", path.string());
    }
}

void Workspace::save_config()
{
    auto path = locus_dir_ / "config.json";
    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("Cannot write config to {}", path.string());
        return;
    }
    f << workspace_config_to_json(config_).dump(2) << '\n';
}

// -- LOCUS.md -----------------------------------------------------------------

void Workspace::load_locus_md()
{
    auto path = root_ / "LOCUS.md";
    if (!fs::exists(path)) {
        spdlog::trace("No LOCUS.md found at workspace root");
        locus_md_.clear();
        return;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        spdlog::warn("Cannot read LOCUS.md at {}", path.string());
        locus_md_.clear();
        return;
    }

    locus_md_.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
    spdlog::info("Loaded LOCUS.md ({} bytes)", locus_md_.size());
}

} // namespace locus

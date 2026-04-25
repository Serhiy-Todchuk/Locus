#include "workspace.h"
#include "core/watcher_pump.h"
#include "core/workspace_lock.h"
#include "database.h"
#include "embedder.h"
#include "embedding_worker.h"
#include "extractors/docx_extractor.h"
#include "extractors/extractor_registry.h"
#include "extractors/html_extractor.h"
#include "extractors/markdown_extractor.h"
#include "extractors/pdf_extractor.h"
#include "extractors/xlsx_extractor.h"
#include "file_watcher.h"
#include "index/index_query.h"
#include "index/indexer.h"
#include "process_registry.h"
#include "reranker.h"

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
    : root_(fs::absolute(root))
{
    if (!fs::is_directory(root_)) {
        throw std::runtime_error("Not a directory: " + root_.string());
    }

    locus_dir_ = root_ / ".locus";
    std::error_code ec;
    fs::create_directories(locus_dir_, ec);
    if (ec) {
        throw std::runtime_error("Cannot create .locus/: " + ec.message());
    }

    // Grab the workspace lock before touching any stateful subsystem — if
    // another Locus instance owns this folder (or an ancestor of it), we
    // must fail cleanly before opening databases or starting threads.
    lock_ = std::make_unique<WorkspaceLock>(root_);

    load_config();
    load_locus_md();

    spdlog::info("Opening main database at {}", (locus_dir_ / "index.db").string());
    main_db_ = std::make_unique<Database>(locus_dir_ / "index.db", DbKind::Main);
    // One-time migration from the pre-split layout.
    main_db_->drop_legacy_semantic_tables();

    spdlog::info("Starting file watcher on {}", root_.string());
    watcher_ = std::make_unique<FileWatcher>(root_, config_.exclude_patterns);
    watcher_->start();

    // Register text extractors for known document formats
    extractors_ = std::make_unique<ExtractorRegistry>();
    extractors_->register_extractor(".md",   std::make_unique<MarkdownExtractor>());
    extractors_->register_extractor(".markdown", std::make_unique<MarkdownExtractor>());
    extractors_->register_extractor(".html", std::make_unique<HtmlExtractor>());
    extractors_->register_extractor(".htm",  std::make_unique<HtmlExtractor>());
    extractors_->register_extractor(".pdf",  std::make_unique<PdfiumExtractor>());
    extractors_->register_extractor(".docx", std::make_unique<DocxExtractor>());
    extractors_->register_extractor(".xlsx", std::make_unique<XlsxExtractor>());

    // Initialise semantic search if enabled (opens vectors.db on success)
    if (config_.semantic_search_enabled) {
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

    indexer_->build_initial();

    // Start embedding worker after initial index (chunks are queued)
    if (embedding_worker_) {
        embedding_worker_->start();
    }

    query_ = std::make_unique<IndexQuery>(*main_db_, vectors_db_.get());

    // Watcher pump: drains the FileWatcher on a background thread and
    // batches events into Indexer::process_events. Replaces the per-frontend
    // pumps that used to live in main.cpp / locus_app.cpp / locus_frame.cpp.
    watcher_pump_ = std::make_unique<WatcherPump>(*watcher_, *indexer_);
    watcher_pump_->start();

    // Background-process registry (S4.I). Empty at startup; tools spawn into it.
    processes_ = std::make_unique<ProcessRegistry>(
        root_, static_cast<std::size_t>(config_.process_output_buffer_kb) * 1024);

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
    // models/ at repo root — up to 4 levels up).
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
        fs::path candidate = base / "models" / config_.embedding_model;
        if (fs::exists(candidate)) {
            model_path = candidate;
            break;
        }
        if (!base.has_parent_path() || base.parent_path() == base) break;
        base = base.parent_path();
    }

    if (model_path.empty()) {
        spdlog::warn("Semantic search enabled but model '{}' not found under {} or its parents",
                     config_.embedding_model, exe_dir.string());
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
        vectors_db_->ensure_vectors_schema(dim, config_.embedding_model);

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
        if (config_.reranker_enabled) {
            fs::path rr_path;
            fs::path b = exe_dir;
            for (int i = 0; i < 6; ++i) {
                fs::path c = b / "models" / config_.reranker_model;
                if (fs::exists(c)) { rr_path = c; break; }
                if (!b.has_parent_path() || b.parent_path() == b) break;
                b = b.parent_path();
            }
            if (rr_path.empty()) {
                spdlog::warn("Reranker enabled but model '{}' not found - "
                             "search will use bi-encoder scores only",
                             config_.reranker_model);
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

static WorkspaceConfig config_from_json(const json& j)
{
    WorkspaceConfig cfg;

    if (j.contains("index")) {
        auto& idx = j["index"];
        if (idx.contains("exclude_patterns"))
            cfg.exclude_patterns = idx["exclude_patterns"].get<std::vector<std::string>>();
        if (idx.contains("max_file_size_kb"))
            cfg.max_file_size_kb = idx["max_file_size_kb"].get<int>();
        if (idx.contains("code_parsing_enabled"))
            cfg.code_parsing_enabled = idx["code_parsing_enabled"].get<bool>();

        if (idx.contains("semantic_search")) {
            auto& ss = idx["semantic_search"];
            if (ss.contains("enabled"))
                cfg.semantic_search_enabled = ss["enabled"].get<bool>();
            if (ss.contains("model"))
                cfg.embedding_model = ss["model"].get<std::string>();
            // 'dimensions' is no longer a config setting - silently ignore
            // legacy entries so old config.json files don't error out.
            if (ss.contains("chunk_size_lines"))
                cfg.chunk_size_lines = ss["chunk_size_lines"].get<int>();
            if (ss.contains("chunk_overlap_lines"))
                cfg.chunk_overlap_lines = ss["chunk_overlap_lines"].get<int>();
            if (ss.contains("reranker_enabled"))
                cfg.reranker_enabled = ss["reranker_enabled"].get<bool>();
            if (ss.contains("reranker_model"))
                cfg.reranker_model = ss["reranker_model"].get<std::string>();
            if (ss.contains("reranker_top_k"))
                cfg.reranker_top_k = ss["reranker_top_k"].get<int>();
        }
    }

    if (j.contains("llm")) {
        auto& llm = j["llm"];
        if (llm.contains("endpoint"))
            cfg.llm_endpoint = llm["endpoint"].get<std::string>();
        if (llm.contains("model"))
            cfg.llm_model = llm["model"].get<std::string>();
        if (llm.contains("temperature"))
            cfg.llm_temperature = llm["temperature"].get<double>();
        if (llm.contains("context_limit"))
            cfg.llm_context_limit = llm["context_limit"].get<int>();
        if (llm.contains("tool_format"))
            cfg.llm_tool_format = llm["tool_format"].get<std::string>();
    }

    if (j.contains("agent")) {
        auto& ag = j["agent"];
        if (ag.contains("tool_manifest_warn_tokens"))
            cfg.tool_manifest_warn_tokens = ag["tool_manifest_warn_tokens"].get<int>();
        if (ag.contains("process_output_buffer_kb"))
            cfg.process_output_buffer_kb = ag["process_output_buffer_kb"].get<int>();
        if (ag.contains("notify_external_changes"))
            cfg.notify_external_changes = ag["notify_external_changes"].get<bool>();
    }

    if (j.contains("tool_approvals") && j["tool_approvals"].is_object()) {
        for (auto it = j["tool_approvals"].begin();
             it != j["tool_approvals"].end(); ++it) {
            if (it.value().is_string()) {
                cfg.tool_approval_policies[it.key()] =
                    policy_from_string(it.value().get<std::string>());
            }
        }
    }

    return cfg;
}

static json config_to_json(const WorkspaceConfig& cfg)
{
    json approvals = json::object();
    for (const auto& [name, policy] : cfg.tool_approval_policies)
        approvals[name] = to_string(policy);

    return {
        {"index", {
            {"exclude_patterns", cfg.exclude_patterns},
            {"max_file_size_kb", cfg.max_file_size_kb},
            {"code_parsing_enabled", cfg.code_parsing_enabled},
            {"semantic_search", {
                {"enabled", cfg.semantic_search_enabled},
                {"model", cfg.embedding_model},
                {"chunk_size_lines", cfg.chunk_size_lines},
                {"chunk_overlap_lines", cfg.chunk_overlap_lines},
                {"reranker_enabled", cfg.reranker_enabled},
                {"reranker_model", cfg.reranker_model},
                {"reranker_top_k", cfg.reranker_top_k}
            }}
        }},
        {"llm", {
            {"endpoint", cfg.llm_endpoint},
            {"model", cfg.llm_model},
            {"temperature", cfg.llm_temperature},
            {"context_limit", cfg.llm_context_limit},
            {"tool_format", cfg.llm_tool_format}
        }},
        {"agent", {
            {"tool_manifest_warn_tokens", cfg.tool_manifest_warn_tokens},
            {"process_output_buffer_kb",  cfg.process_output_buffer_kb},
            {"notify_external_changes",   cfg.notify_external_changes}
        }},
        {"tool_approvals", approvals}
    };
}

void Workspace::load_config()
{
    auto path = locus_dir_ / "config.json";

    if (fs::exists(path)) {
        std::ifstream f(path);
        if (f.is_open()) {
            try {
                json j = json::parse(f);
                config_ = config_from_json(j);
                spdlog::info("Loaded config from {}", path.string());
                return;
            } catch (const json::exception& e) {
                spdlog::warn("Failed to parse config.json, using defaults: {}", e.what());
            }
        }
    }

    // First run or parse failure: write defaults
    config_ = WorkspaceConfig{};
    save_config();
    spdlog::info("Created default config at {}", path.string());
}

void Workspace::save_config()
{
    auto path = locus_dir_ / "config.json";
    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("Cannot write config to {}", path.string());
        return;
    }
    f << config_to_json(config_).dump(2) << '\n';
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

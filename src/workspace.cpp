#include "workspace.h"
#include "database.h"
#include "extractors/extractor_registry.h"
#include "extractors/html_extractor.h"
#include "extractors/markdown_extractor.h"
#include "file_watcher.h"
#include "index_query.h"
#include "indexer.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <stdexcept>

namespace locus {

using json = nlohmann::json;

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

    load_config();
    load_locus_md();

    spdlog::info("Opening database at {}", (locus_dir_ / "index.db").string());
    db_ = std::make_unique<Database>(locus_dir_ / "index.db");

    spdlog::info("Starting file watcher on {}", root_.string());
    watcher_ = std::make_unique<FileWatcher>(root_, config_.exclude_patterns);
    watcher_->start();

    // Register text extractors for known document formats
    extractors_ = std::make_unique<ExtractorRegistry>();
    extractors_->register_extractor(".md",   std::make_unique<MarkdownExtractor>());
    extractors_->register_extractor(".markdown", std::make_unique<MarkdownExtractor>());
    extractors_->register_extractor(".html", std::make_unique<HtmlExtractor>());
    extractors_->register_extractor(".htm",  std::make_unique<HtmlExtractor>());

    spdlog::info("Building workspace index");
    indexer_ = std::make_unique<Indexer>(*db_, root_, config_, *extractors_);
    indexer_->build_initial();

    query_ = std::make_unique<IndexQuery>(*db_);

    spdlog::info("Workspace opened: {}", root_.string());
}

Workspace::~Workspace()
{
    if (watcher_) {
        watcher_->stop();
    }
    spdlog::info("Workspace closed: {}", root_.string());
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
            if (ss.contains("dimensions"))
                cfg.embedding_dimensions = ss["dimensions"].get<int>();
            if (ss.contains("chunk_size_lines"))
                cfg.chunk_size_lines = ss["chunk_size_lines"].get<int>();
            if (ss.contains("chunk_overlap_lines"))
                cfg.chunk_overlap_lines = ss["chunk_overlap_lines"].get<int>();
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
    }

    return cfg;
}

static json config_to_json(const WorkspaceConfig& cfg)
{
    return {
        {"index", {
            {"exclude_patterns", cfg.exclude_patterns},
            {"max_file_size_kb", cfg.max_file_size_kb},
            {"code_parsing_enabled", cfg.code_parsing_enabled},
            {"semantic_search", {
                {"enabled", cfg.semantic_search_enabled},
                {"model", cfg.embedding_model},
                {"dimensions", cfg.embedding_dimensions},
                {"chunk_size_lines", cfg.chunk_size_lines},
                {"chunk_overlap_lines", cfg.chunk_overlap_lines}
            }}
        }},
        {"llm", {
            {"endpoint", cfg.llm_endpoint},
            {"model", cfg.llm_model},
            {"temperature", cfg.llm_temperature},
            {"context_limit", cfg.llm_context_limit}
        }}
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

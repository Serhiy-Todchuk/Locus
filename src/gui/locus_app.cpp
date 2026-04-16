#include "locus_app.h"
#include "locus_frame.h"
#include "../tools.h"
#include "../indexer.h"
#include "../file_watcher.h"
#include "../index_query.h"
#include "../system_prompt.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <wx/cmdline.h>
#include <wx/stdpaths.h>

#include <filesystem>
#include <thread>
#include <chrono>
#include <clocale>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace locus {

// wxWidgets macro: defines WinMain and creates the LocusApp instance.
wxIMPLEMENT_APP(LocusApp);

bool LocusApp::OnInit()
{
    if (!wxApp::OnInit())
        return false;

    // Follow OS light/dark theme (Windows 10+).
    MSWEnableDarkMode();

    // UTF-8 support.
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setlocale(LC_ALL, ".UTF-8");

    // Single-instance enforcement.
    instance_checker_ = std::make_unique<wxSingleInstanceChecker>("Locus-SingleInstance");
    if (instance_checker_->IsAnotherRunning()) {
        wxMessageBox("Locus is already running.", "Locus",
                     wxOK | wxICON_INFORMATION);
        return false;
    }

    // Parse command-line args.
    // Usage: locus_gui [workspace_path] [--endpoint URL] [--model NAME]
    std::string workspace_path_str;
    std::string endpoint = "http://127.0.0.1:1234";
    std::string model;
    int context = 0;

    if (argc > 1)
        workspace_path_str = argv[1].ToStdString();

    for (int i = 2; i < argc; ++i) {
        wxString a = argv[i];
        if (a == "--endpoint" && i + 1 < argc)
            endpoint = argv[++i].ToStdString();
        else if (a == "--model" && i + 1 < argc)
            model = argv[++i].ToStdString();
        else if (a == "--context" && i + 1 < argc) {
            try { context = std::stoi(argv[++i].ToStdString()); }
            catch (...) { context = 0; }
        }
    }

    // If no workspace path given, prompt user.
    if (workspace_path_str.empty()) {
        wxDirDialog dlg(nullptr, "Select workspace folder",
                        wxStandardPaths::Get().GetDocumentsDir(),
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return false;
        workspace_path_str = dlg.GetPath().ToStdString();
    }

    // Resolve workspace path.
    std::error_code ec;
    fs::path ws_path = fs::absolute(workspace_path_str, ec);
    if (ec || !fs::is_directory(ws_path)) {
        wxMessageBox(wxString::Format("Not a directory: %s", workspace_path_str),
                     "Locus", wxOK | wxICON_ERROR);
        return false;
    }

    // Ensure .locus/ and init logging.
    fs::path locus_dir = ws_path / ".locus";
    fs::create_directories(locus_dir, ec);
    if (ec) {
        wxMessageBox("Cannot create .locus/ directory", "Locus",
                     wxOK | wxICON_ERROR);
        return false;
    }
    init_logging(locus_dir);

    spdlog::info("Locus GUI starting");
    spdlog::info("Workspace: {}", ws_path.string());

    try {
        // Open workspace.
        workspace_ = std::make_unique<Workspace>(ws_path);

        if (!workspace_->locus_md().empty())
            spdlog::info("LOCUS.md loaded ({} bytes)", workspace_->locus_md().size());

        // Drain startup file events.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::vector<FileEvent> events;
        size_t n = workspace_->file_watcher().drain(events);
        if (n > 0) {
            spdlog::info("File watcher: drained {} events, updating index", n);
            workspace_->indexer().process_events(events);
        }

        auto& st = workspace_->indexer().stats();
        spdlog::info("Index: {} files ({} text, {} binary), {} symbols, {} headings",
                     st.files_total, st.files_indexed, st.files_binary,
                     st.symbols_total, st.headings_total);

        // LLM client.
        LLMConfig llm_cfg;
        llm_cfg.base_url      = endpoint;
        llm_cfg.model         = model;
        llm_cfg.context_limit = context;
        llm_ = create_llm_client(llm_cfg);

        auto model_info = llm_->query_model_info();
        if (!model_info.id.empty() && llm_cfg.model.empty()) {
            llm_cfg.model = model_info.id;
            spdlog::info("Model auto-detected: {}", llm_cfg.model);
        }
        if (context > 0) {
            llm_cfg.context_limit = context;
        } else if (model_info.context_length > 0) {
            llm_cfg.context_limit = model_info.context_length;
        } else {
            llm_cfg.context_limit = 8192;
        }
        spdlog::info("Context limit: {}", llm_cfg.context_limit);

        // Tool system.
        tools_ = std::make_unique<ToolRegistry>();
        register_builtin_tools(*tools_);
        spdlog::info("Tools: {} registered", tools_->all().size());

        // Agent core.
        WorkspaceContext ws_ctx{ws_path, &workspace_->query(), workspace_->embedding_worker()};
        WorkspaceMetadata ws_meta;
        ws_meta.root          = ws_path;
        ws_meta.file_count    = static_cast<int>(st.files_total);
        ws_meta.symbol_count  = static_cast<int>(st.symbols_total);
        ws_meta.heading_count = static_cast<int>(st.headings_total);

        fs::path sessions_dir = locus_dir / "sessions";
        agent_ = std::make_unique<AgentCore>(
            *llm_, *tools_, ws_ctx,
            workspace_->locus_md(), ws_meta, llm_cfg, sessions_dir);

        agent_->start();

        // Create the main frame (wxWidgets owns it after Show).
        frame_ = new LocusFrame(*agent_, *workspace_);
        frame_->Show(true);

    } catch (const std::exception& ex) {
        spdlog::error("Fatal: {}", ex.what());
        wxMessageBox(wxString::Format("Startup failed: %s", ex.what()),
                     "Locus", wxOK | wxICON_ERROR);
        return false;
    }

    return true;
}

int LocusApp::OnExit()
{
    spdlog::info("Locus GUI shutting down");

    if (agent_) {
        agent_->stop();
        agent_.reset();
    }
    tools_.reset();
    llm_.reset();
    workspace_.reset();

    spdlog::shutdown();
    return wxApp::OnExit();
}

void LocusApp::init_logging(const std::filesystem::path& locus_dir)
{
    try {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (locus_dir / "locus.log").string(),
            10 * 1024 * 1024, 3);
        file_sink->set_level(spdlog::level::trace);

        auto logger = std::make_shared<spdlog::logger>("locus",
            spdlog::sinks_init_list{file_sink});
        logger->set_level(spdlog::level::trace);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [t:%t] [%^%l%$] %v");

        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::warn);
    }
    catch (const spdlog::spdlog_ex& ex) {
        wxMessageBox(wxString::Format("Log init failed: %s", ex.what()),
                     "Locus", wxOK | wxICON_ERROR);
    }
}

} // namespace locus

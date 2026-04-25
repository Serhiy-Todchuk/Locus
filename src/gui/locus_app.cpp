#include "locus_app.h"
#include "locus_frame.h"
#include "recent_workspaces.h"
#include "../tools/tools.h"
#include "../indexer.h"
#include "../index_query.h"
#include "../agent/system_prompt.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <wx/cmdline.h>
#include <wx/stdpaths.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <clocale>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#if defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#endif

namespace fs = std::filesystem;

namespace locus {

#if defined(_WIN32) && defined(_DEBUG)
// Baseline snapshot taken after OnInit completes. At shutdown we only dump
// allocations made *after* this checkpoint, so lazy one-shot statics from the
// STL/ICU (std::chrono::tzdb, locale data, etc.) and from wxWidgets internals
// don't drown out real leaks.
static _CrtMemState s_startup_mem_state{};
static bool         s_startup_checkpoint_taken = false;
#endif

// wxWidgets macro: defines WinMain and creates the LocusApp instance.
wxIMPLEMENT_APP(LocusApp);

// If opening a folder that has no .locus/config.json yet, ask the user whether
// to enable semantic search. Write a minimal config.json capturing the choice
// so Workspace picks it up during construction. No-op when config.json already
// exists (not a first-time open).
static void prompt_semantic_search_if_first_open(const fs::path& locus_dir)
{
    auto cfg_path = locus_dir / "config.json";
    if (fs::exists(cfg_path))
        return;

    int reply = wxMessageBox(
        "Enable semantic search for this workspace?\n\n"
        "Semantic search uses a local embedding model to find files by meaning, "
        "not just keywords. It takes extra CPU and disk during indexing.\n\n"
        "You can change this later in Settings.",
        "Locus \u2014 New Workspace",
        wxYES_NO | wxICON_QUESTION);

    bool enabled = (reply == wxYES);

    try {
        nlohmann::json j;
        j["index"]["semantic_search"]["enabled"] = enabled;
        std::ofstream f(cfg_path);
        f << j.dump(2) << '\n';
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to pre-seed config.json: {}", ex.what());
    }
}

bool LocusApp::OnInit()
{
    if (!wxApp::OnInit())
        return false;

#if defined(_WIN32) && defined(_DEBUG)
    // Disable the CRT's automatic leak dump at exit; we emit our own dump
    // filtered against a post-init checkpoint (see OnExit).
    {
        int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
        flags &= ~_CRTDBG_LEAK_CHECK_DF;
        _CrtSetDbgFlag(flags);
    }
    // Force lazy IANA tzdb load now so its ~90 never-freed blocks land before
    // the checkpoint rather than after, where they'd masquerade as leaks.
    try { (void)std::chrono::current_zone(); } catch (...) {}
    // LOCUS_BREAK_ALLOC=<n> breaks in the debugger at CRT allocation number n,
    // so the call stack pinpoints the leaking caller. Comma-separated for
    // multiple targets (only the last one sticks, so iterate).
    if (const char* env = std::getenv("LOCUS_BREAK_ALLOC")) {
        long n = 0;
        for (const char* p = env; *p; ) {
            char* end = nullptr;
            long v = std::strtol(p, &end, 10);
            if (end == p) break;
            if (v > 0) { n = v; _CrtSetBreakAlloc(v); }
            p = (*end == ',') ? end + 1 : end;
        }
        if (n > 0)
            spdlog::info("Debug: break on CRT allocation #{}", n);
    }
#endif

    // Follow OS light/dark theme (Windows 10+).
    MSWEnableDarkMode();

    // UTF-8 support.
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setlocale(LC_ALL, ".UTF-8");

    // Single-instance enforcement is now workspace-scoped — WorkspaceLock
    // grabs an exclusive file lock inside .locus/ when the Workspace opens,
    // so two Locus GUIs on *different* folders can coexist, but two on the
    // same folder (or a nested one) fail loudly.

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
        // Default to last used workspace, fall back to Documents.
        std::string default_dir = RecentWorkspaces::last();
        if (default_dir.empty())
            default_dir = wxStandardPaths::Get().GetDocumentsDir().ToStdString();

        wxDirDialog dlg(nullptr, "Select workspace folder",
                        default_dir,
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

    // Record in recent workspaces list.
    RecentWorkspaces::add(ws_path.string());

    spdlog::info("Locus GUI starting");
    spdlog::info("Workspace: {}", ws_path.string());

    prompt_semantic_search_if_first_open(locus_dir);

    try {
        // Open workspace.
        workspace_ = std::make_unique<Workspace>(ws_path);

        if (!workspace_->locus_md().empty())
            spdlog::info("LOCUS.md loaded ({} bytes)", workspace_->locus_md().size());

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
        WorkspaceMetadata ws_meta;
        ws_meta.root          = ws_path;
        ws_meta.file_count    = static_cast<int>(st.files_total);
        ws_meta.symbol_count  = static_cast<int>(st.symbols_total);
        ws_meta.heading_count = static_cast<int>(st.headings_total);

        fs::path sessions_dir    = locus_dir / "sessions";
        fs::path checkpoints_dir = locus_dir / "checkpoints";
        agent_ = std::make_unique<AgentCore>(
            *llm_, *tools_, *workspace_,
            workspace_->locus_md(), ws_meta, llm_cfg,
            sessions_dir, checkpoints_dir);

        workspace_->indexer().on_activity =
            [agent = agent_.get()](const std::string& s, const std::string& d) {
                agent->emit_index_event(s, d);
            };

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

#if defined(_WIN32) && defined(_DEBUG)
    _CrtMemCheckpoint(&s_startup_mem_state);
    s_startup_checkpoint_taken = true;
#endif

    return true;
}

void LocusApp::open_workspace(const std::string& path)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path ws_path = fs::absolute(path, ec);
    if (ec || !fs::is_directory(ws_path)) {
        wxMessageBox(wxString::Format("Not a directory: %s", path),
                     "Locus", wxOK | wxICON_ERROR);
        return;
    }

    // Record in recent workspaces list.
    RecentWorkspaces::add(ws_path.string());

    // Tear down current subsystems in reverse order.
    spdlog::info("Switching workspace to: {}", ws_path.string());

    // Close the old frame (triggers ~LocusFrame which unregisters frontend).
    if (frame_) {
        frame_->Destroy();
        frame_ = nullptr;
    }

    if (agent_) {
        agent_->stop();
        agent_.reset();
    }
    tools_.reset();
    llm_.reset();
    workspace_.reset();

    // Set up .locus/ dir and logging for the new workspace.
    fs::path locus_dir = ws_path / ".locus";
    fs::create_directories(locus_dir, ec);
    if (ec) {
        wxMessageBox("Cannot create .locus/ directory", "Locus",
                     wxOK | wxICON_ERROR);
        return;
    }
    init_logging(locus_dir);

    prompt_semantic_search_if_first_open(locus_dir);

    try {
        workspace_ = std::make_unique<Workspace>(ws_path);

        if (!workspace_->locus_md().empty())
            spdlog::info("LOCUS.md loaded ({} bytes)", workspace_->locus_md().size());

        auto& st = workspace_->indexer().stats();
        spdlog::info("Index: {} files ({} text, {} binary), {} symbols, {} headings",
                     st.files_total, st.files_indexed, st.files_binary,
                     st.symbols_total, st.headings_total);

        // Reuse LLM settings from workspace config if available,
        // otherwise fall back to defaults.
        LLMConfig llm_cfg;
        auto& wc = workspace_->config();
        if (!wc.llm_endpoint.empty())
            llm_cfg.base_url = wc.llm_endpoint;
        if (!wc.llm_model.empty())
            llm_cfg.model = wc.llm_model;
        if (wc.llm_context_limit > 0)
            llm_cfg.context_limit = wc.llm_context_limit;
        if (wc.llm_temperature > 0.0)
            llm_cfg.temperature = wc.llm_temperature;

        llm_ = create_llm_client(llm_cfg);

        auto model_info = llm_->query_model_info();
        if (!model_info.id.empty() && llm_cfg.model.empty()) {
            llm_cfg.model = model_info.id;
            spdlog::info("Model auto-detected: {}", llm_cfg.model);
        }
        if (llm_cfg.context_limit <= 0) {
            if (model_info.context_length > 0)
                llm_cfg.context_limit = model_info.context_length;
            else
                llm_cfg.context_limit = 8192;
        }
        spdlog::info("Context limit: {}", llm_cfg.context_limit);

        tools_ = std::make_unique<ToolRegistry>();
        register_builtin_tools(*tools_);
        spdlog::info("Tools: {} registered", tools_->all().size());

        WorkspaceMetadata ws_meta;
        ws_meta.root          = ws_path;
        ws_meta.file_count    = static_cast<int>(st.files_total);
        ws_meta.symbol_count  = static_cast<int>(st.symbols_total);
        ws_meta.heading_count = static_cast<int>(st.headings_total);

        fs::path sessions_dir    = locus_dir / "sessions";
        fs::path checkpoints_dir = locus_dir / "checkpoints";
        agent_ = std::make_unique<AgentCore>(
            *llm_, *tools_, *workspace_,
            workspace_->locus_md(), ws_meta, llm_cfg,
            sessions_dir, checkpoints_dir);

        workspace_->indexer().on_activity =
            [agent = agent_.get()](const std::string& s, const std::string& d) {
                agent->emit_index_event(s, d);
            };

        agent_->start();

        frame_ = new LocusFrame(*agent_, *workspace_);
        frame_->Show(true);

        spdlog::info("Workspace switch complete");

    } catch (const std::exception& ex) {
        spdlog::error("Workspace switch failed: {}", ex.what());
        wxMessageBox(wxString::Format("Failed to open workspace: %s", ex.what()),
                     "Locus", wxOK | wxICON_ERROR);
    }
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
    int rc = wxApp::OnExit();

#if defined(_WIN32) && defined(_DEBUG)
    if (s_startup_checkpoint_taken) {
        _CrtMemState now{}, diff{};
        _CrtMemCheckpoint(&now);
        if (_CrtMemDifference(&diff, &s_startup_mem_state, &now)) {
            OutputDebugStringA("Locus: leaks since post-init checkpoint:\n");
            _CrtMemDumpStatistics(&diff);
            _CrtMemDumpAllObjectsSince(&s_startup_mem_state);
        } else {
            OutputDebugStringA("Locus: no leaks since post-init checkpoint.\n");
        }
    }
#endif

    return rc;
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

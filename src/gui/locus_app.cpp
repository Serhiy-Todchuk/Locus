#include "locus_app.h"
#include "locus_frame.h"
#include "recent_workspaces.h"

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

    // Seed LLM config from the user's startup args. Empty/zero fields are
    // filled in by LocusSession from .locus/config.json + the live model
    // probe — see src/core/locus_session.cpp.
    LLMConfig seed;
    seed.base_url      = endpoint;
    seed.model         = model;
    seed.context_limit = context;

    if (!spawn_session(ws_path, seed))
        return false;

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

    spdlog::info("Switching workspace to: {}", ws_path.string());

    // wxApp shuts down when the last top-level window is destroyed (the
    // default SetExitOnFrameDelete behaviour). The swap below has a brief
    // window with zero TLWs -- DeletePendingObjects() destroys the old
    // frame before spawn_session() creates the new one. Without disabling
    // the auto-exit, that gap trips wxApp::ExitMainLoop, so the new frame
    // we then spawn is shown for a few seconds inside an already-exiting
    // event loop and gets ripped out by DeleteAllTLWs after OnExit has
    // reset the session, which lands ~LocusFrame on a freed Workspace.
    SetExitOnFrameDelete(false);

    // Close the old frame. wxWindow::Destroy() queues deletion for the next
    // idle event, which may fire AFTER session_.reset() below -- so we
    // detach the frame's references into the session synchronously here,
    // while workspace_ + agent_ are still alive. ~LocusFrame is then safe
    // to run whenever wx finally gets to it (the destructor calls
    // detach_from_session() too, but it's a no-op once we've already done it).
    if (frame_) {
        frame_->detach_from_session();
        frame_->Destroy();
        frame_ = nullptr;
    }

    // Drain the pending-delete queue now so the old frame is fully gone
    // before session_.reset() frees the MetricsAggregator that the old
    // frame's MetricsView 1 s timer still references. The detach above
    // already made ~LocusFrame safe; this prevents a different timer-vs-
    // freed-mutex race during the modal dialog below.
    DeletePendingObjects();

    // ~LocusSession joins the agent thread, then drops tools / llm /
    // workspace in reverse declaration order — same teardown sequence as
    // the old hand-wired code.
    session_.reset();

    // Set up .locus/ dir and logging for the new workspace.
    fs::path locus_dir = ws_path / ".locus";
    fs::create_directories(locus_dir, ec);
    if (ec) {
        wxMessageBox("Cannot create .locus/ directory", "Locus",
                     wxOK | wxICON_ERROR);
        SetExitOnFrameDelete(true);
        return;
    }
    init_logging(locus_dir);

    prompt_semantic_search_if_first_open(locus_dir);

    // Switching workspaces from the menu doesn't carry CLI overrides; the
    // new session pulls everything from .locus/config.json + the live
    // model probe.
    if (spawn_session(ws_path, LLMConfig{}))
        spdlog::info("Workspace switch complete");

    // Restore the default so closing the new frame eventually exits the app.
    SetExitOnFrameDelete(true);
}

bool LocusApp::spawn_session(const std::filesystem::path& ws_path,
                             const LLMConfig&             seed_cfg)
{
    try {
        session_ = std::make_unique<LocusSession>(ws_path, seed_cfg);
    } catch (const std::exception& ex) {
        spdlog::error("Session start failed: {}", ex.what());
        wxMessageBox(wxString::Format("Failed to open workspace: %s", ex.what()),
                     "Locus", wxOK | wxICON_ERROR);
        session_.reset();
        return false;
    }

    frame_ = new LocusFrame(session_->agent(), session_->workspace());
    frame_->Show(true);
    return true;
}

int LocusApp::OnExit()
{
    spdlog::info("Locus GUI shutting down");

    // ~LocusSession stops the agent thread and tears down tools/llm/workspace
    // in reverse declaration order.
    session_.reset();

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

#include "locus_frame.h"
#include "locus_app.h"
#include "recent_workspaces.h"

#include "../embedding_worker.h"
#include "../indexer.h"

#include <spdlog/spdlog.h>

#include <wx/aboutdlg.h>
#include <wx/dirdlg.h>
#include <wx/stdpaths.h>

namespace locus {

enum {
    ID_MENU_QUIT = wxID_EXIT,
    ID_MENU_ABOUT = wxID_ABOUT,
    ID_MENU_RESET = wxID_HIGHEST + 200,
    ID_MENU_COMPACT,
    ID_MENU_SAVE_SESSION,
    ID_MENU_OPEN_WORKSPACE,
    ID_MENU_SETTINGS,
    ID_MENU_RECENT_BASE = wxID_HIGHEST + 300,  // 300..309 for recent workspaces
};

wxBEGIN_EVENT_TABLE(LocusFrame, wxFrame)
    EVT_CLOSE(LocusFrame::on_close)
    EVT_ICONIZE(LocusFrame::on_iconize)
wxEND_EVENT_TABLE()

LocusFrame::LocusFrame(AgentCore& agent, Workspace& workspace)
    : wxFrame(nullptr, wxID_ANY, "Locus",
              wxDefaultPosition, wxSize(1200, 800))
    , agent_(agent)
    , workspace_(workspace)
{
    // AUI manager
    aui_.SetManagedWindow(this);

    create_menu_bar();
    create_status_bar();
    setup_aui_layout();

    aui_.Update();

    // System tray icon.
    tray_ = std::make_unique<LocusTray>(this);

    // Thread bridge: WxFrontend posts events to this frame.
    wx_frontend_ = std::make_unique<WxFrontend>(this);
    agent_.register_frontend(wx_frontend_.get());

    // Bind agent thread events.
    Bind(EVT_AGENT_TURN_START,    &LocusFrame::on_agent_turn_start,    this);
    Bind(EVT_AGENT_TOKEN,         &LocusFrame::on_agent_token,         this);
    Bind(EVT_AGENT_TOOL_PENDING,  &LocusFrame::on_agent_tool_pending,  this);
    Bind(EVT_AGENT_TOOL_RESULT,   &LocusFrame::on_agent_tool_result,   this);
    Bind(EVT_AGENT_TURN_COMPLETE, &LocusFrame::on_agent_turn_complete, this);
    Bind(EVT_AGENT_CONTEXT_METER, &LocusFrame::on_agent_context_meter, this);
    Bind(EVT_AGENT_COMPACTION,    &LocusFrame::on_agent_compaction,    this);
    Bind(EVT_AGENT_SESSION_RESET, &LocusFrame::on_agent_session_reset, this);
    Bind(EVT_AGENT_ERROR,         &LocusFrame::on_agent_error,         this);
    Bind(EVT_AGENT_EMBEDDING_PROGRESS, &LocusFrame::on_agent_embedding_progress, this);

    // Show LOCUS.md token cost if present.
    if (!workspace_.locus_md().empty()) {
        // Rough estimate: ~4 chars per token.
        int locus_tokens = static_cast<int>(workspace_.locus_md().size()) / 4;
        chat_panel_->set_locus_md_tokens(locus_tokens);
    }

    // Wire embedding worker progress to the WxFrontend thread bridge.
    if (workspace_.embedding_worker()) {
        workspace_.embedding_worker()->on_progress = [this](int done, int total) {
            wx_frontend_->on_embedding_progress(done, total);
        };
    }

    // Populate index stats in the file tree panel.
    refresh_index_stats();

    Centre();
    spdlog::info("LocusFrame created");
}

LocusFrame::~LocusFrame()
{
    // Detach the embedding-worker progress callback before tearing down
    // wx_frontend_ — the worker runs on its own thread and may still fire
    // progress events after the frame starts destructing.
    if (workspace_.embedding_worker())
        workspace_.embedding_worker()->on_progress = nullptr;

    agent_.unregister_frontend(wx_frontend_.get());
    aui_.UnInit();
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

void LocusFrame::create_menu_bar()
{
    auto* file_menu = new wxMenu;
    file_menu->Append(ID_MENU_OPEN_WORKSPACE, "Open Workspace...\tCtrl+O");

    // Recent Workspaces submenu.
    recent_menu_ = new wxMenu;
    rebuild_recent_menu();
    file_menu->AppendSubMenu(recent_menu_, "Recent Workspaces");

    file_menu->Append(ID_MENU_SETTINGS,       "Settings...\tCtrl+,");
    file_menu->AppendSeparator();
    file_menu->Append(ID_MENU_QUIT, "Quit\tCtrl+Q");

    auto* session_menu = new wxMenu;
    session_menu->Append(ID_MENU_RESET,        "Reset Conversation\tCtrl+R");
    session_menu->Append(ID_MENU_COMPACT,       "Compact Context");
    session_menu->Append(ID_MENU_SAVE_SESSION,  "Save Session\tCtrl+S");

    auto* help_menu = new wxMenu;
    help_menu->Append(ID_MENU_ABOUT, "About...");

    auto* menu_bar = new wxMenuBar;
    menu_bar->Append(file_menu,    "File");
    menu_bar->Append(session_menu, "Session");
    menu_bar->Append(help_menu,    "Help");
    SetMenuBar(menu_bar);

    // Menu event handlers.
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(false); }, ID_MENU_QUIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxDirDialog dlg(this, "Select workspace folder",
            wxStandardPaths::Get().GetDocumentsDir(),
            wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            std::string path = dlg.GetPath().ToStdString();
            // Defer the switch so this handler returns before the frame is destroyed.
            CallAfter([path]() {
                wxGetApp().open_workspace(path);
            });
        }
    }, ID_MENU_OPEN_WORKSPACE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        agent_.reset_conversation();
    }, ID_MENU_RESET);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        show_compaction_dialog();
    }, ID_MENU_COMPACT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        auto id = agent_.save_session();
        SetStatusText(wxString::Format("Session saved: %s", id));
    }, ID_MENU_SAVE_SESSION);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        show_settings_dialog();
    }, ID_MENU_SETTINGS);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        wxAboutDialogInfo info;
        info.SetName("Locus");
        info.SetVersion("0.1.0");
        info.SetDescription("Local LLM Agent Assistant");
        info.SetCopyright("MIT License");
        wxAboutBox(info, this);
    }, ID_MENU_ABOUT);
}

void LocusFrame::rebuild_recent_menu()
{
    // Clear existing items.
    while (recent_menu_->GetMenuItemCount() > 0)
        recent_menu_->Delete(recent_menu_->FindItemByPosition(0));

    auto entries = RecentWorkspaces::load();
    if (entries.empty()) {
        recent_menu_->Append(wxID_ANY, "(none)")->Enable(false);
        return;
    }

    int id = ID_MENU_RECENT_BASE;
    for (size_t i = 0; i < entries.size() && i < RecentWorkspaces::k_max_entries; ++i, ++id) {
        recent_menu_->Append(id, wxString::FromUTF8(entries[i]));
        Bind(wxEVT_MENU, [this, path = entries[i]](wxCommandEvent&) {
            CallAfter([path]() {
                wxGetApp().open_workspace(path);
            });
        }, id);
    }
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void LocusFrame::create_status_bar()
{
    CreateStatusBar(2);
    SetStatusText("Ready", 0);
    SetStatusText("ctx: 0/0", 1);
}

// ---------------------------------------------------------------------------
// AUI layout: 3-pane shell
// ---------------------------------------------------------------------------

void LocusFrame::setup_aui_layout()
{
    // Left sidebar: file tree with index stats.
    file_tree_panel_ = new FileTreePanel(this, workspace_.query(),
        workspace_.root().string(),
        [this](const std::string& path) {
            spdlog::trace("File selected in tree: {}", path);
            SetStatusText(wxString::FromUTF8(path), 0);
        });

    // Center chat panel.
    chat_panel_ = new ChatPanel(this,
        [this](const std::string& msg) { agent_.send_message(msg); },
        [this]() { show_compaction_dialog(); },
        [this]() { agent_.cancel_turn(); });

    // Tool approval panel — slides in when a tool call needs approval.
    approval_panel_ = new ToolApprovalPanel(this,
        [this](const std::string& call_id, ToolDecision decision,
               const nlohmann::json& modified_args) {
            agent_.tool_decision(call_id, decision, modified_args);
        });

    // Right detail panel (placeholder for future file details / outline).
    detail_panel_ = new wxPanel(this, wxID_ANY);
    detail_panel_->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

    // Add panes.
    aui_.AddPane(file_tree_panel_, wxAuiPaneInfo()
        .Name("sidebar").Caption("Files")
        .Left().MinSize(180, -1).BestSize(240, -1)
        .CloseButton(true).PinButton(true));

    aui_.AddPane(chat_panel_, wxAuiPaneInfo()
        .Name("chat").Caption("Chat")
        .CenterPane());

    aui_.AddPane(approval_panel_, wxAuiPaneInfo()
        .Name("approval").Caption("Tool Approval")
        .Bottom().MinSize(-1, 180).BestSize(-1, 220)
        .CloseButton(false).PinButton(false)
        .Hide());  // shown dynamically when a tool call needs approval

    aui_.AddPane(detail_panel_, wxAuiPaneInfo()
        .Name("details").Caption("Details")
        .Right().MinSize(200, -1).BestSize(300, -1)
        .CloseButton(true).PinButton(true));
}

// ---------------------------------------------------------------------------
// Compaction dialog
// ---------------------------------------------------------------------------

void LocusFrame::show_compaction_dialog()
{
    int used  = static_cast<int>(agent_.history().estimate_tokens());
    int limit = agent_.context_limit();

    CompactionDialog dlg(this, used, limit, agent_.history());
    if (dlg.ShowModal() == wxID_OK) {
        auto choice = dlg.result();
        if (choice.made) {
            agent_.compact_context(choice.strategy, choice.drop_n);
            SetStatusText("Context compacted", 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------

void LocusFrame::show_settings_dialog()
{
    SettingsDialog dlg(this, workspace_.config());
    if (dlg.ShowModal() == wxID_OK && dlg.config_changed()) {
        workspace_.save_config();
        SetStatusText("Settings saved", 0);

        if (dlg.llm_changed()) {
            wxMessageBox(
                "LLM settings changed.\nRestart Locus to apply new endpoint/model settings.",
                "Settings", wxOK | wxICON_INFORMATION, this);
        }

        if (dlg.semantic_changed()) {
            // Always disable first (handles model name change while enabled)
            workspace_.disable_semantic_search();

            if (workspace_.config().semantic_search_enabled) {
                if (!workspace_.enable_semantic_search()) {
                    wxMessageBox(
                        "Could not enable semantic search.\n"
                        "Check that the GGUF model file exists in the models/ folder\n"
                        "next to the Locus executable.",
                        "Semantic Search", wxOK | wxICON_WARNING, this);
                }
            }
        }

        spdlog::info("Settings saved to config.json");
    }
}

// ---------------------------------------------------------------------------
// Index stats
// ---------------------------------------------------------------------------

void LocusFrame::refresh_index_stats()
{
    auto& st = workspace_.indexer().stats();
    file_tree_panel_->set_index_stats(
        st.files_total, st.symbols_total, st.headings_total);
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

void LocusFrame::on_close(wxCloseEvent& evt)
{
    // The embedding worker runs on its own thread and fires on_progress
    // callbacks into this frame.  Stop it here — before destruction — so no
    // callback can fire into freed memory.  stop() joins the thread.
    if (auto* ew = workspace_.embedding_worker()) {
        ew->on_progress = nullptr;
        ew->stop();
    }

    // Remove tray icon before destroying frame.
    if (tray_)
        tray_->RemoveIcon();
    tray_.reset();

    evt.Skip();  // proceed with destruction
}

void LocusFrame::on_iconize(wxIconizeEvent& evt)
{
    if (evt.IsIconized()) {
        // Minimize to tray: hide the window, tray icon remains.
        Hide();
    }
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Agent thread events — UI thread handlers
// ---------------------------------------------------------------------------

void LocusFrame::on_agent_turn_start(wxThreadEvent& /*evt*/)
{
    SetStatusText("Agent working...", 0);
    if (tray_) tray_->set_state(LocusTray::State::active);
    chat_panel_->on_turn_start();
}

void LocusFrame::on_agent_token(wxThreadEvent& evt)
{
    chat_panel_->on_token(evt.GetString());
}

void LocusFrame::on_agent_tool_pending(wxThreadEvent& evt)
{
    try {
        auto payload = nlohmann::json::parse(evt.GetString().ToStdString());
        std::string tool    = payload.value("tool", "");
        std::string call_id = payload.value("id", "");
        std::string preview = payload.value("preview", "");
        nlohmann::json args = payload.value("args", nlohmann::json::object());

        // Show tool call inline in chat.
        chat_panel_->on_tool_pending(
            wxString::FromUTF8(tool), wxString::FromUTF8(preview));

        if (tool == "ask_user") {
            // Pop-up modal — disappears immediately after answer/cancel.
            std::string question = args.value("question", "");
            AskUserDialog dlg(this, question);
            if (dlg.ShowModal() == wxID_OK) {
                nlohmann::json modified = args;
                modified["response"] = dlg.response();
                agent_.tool_decision(call_id, ToolDecision::modify, modified);
            } else {
                agent_.tool_decision(call_id, ToolDecision::reject, {});
            }
        } else {
            // Show approval panel and update AUI.
            approval_panel_->show_tool_call(call_id, tool, args, preview);
            aui_.GetPane("approval").Show();
            aui_.Update();
        }
    } catch (const std::exception& ex) {
        spdlog::warn("Failed to parse tool_pending payload: {}", ex.what());
    }
}

void LocusFrame::on_agent_tool_result(wxThreadEvent& evt)
{
    // Hide approval panel (tool was executed).
    if (approval_panel_->IsShown()) {
        approval_panel_->dismiss();
        aui_.GetPane("approval").Hide();
        aui_.Update();
    }

    try {
        auto payload = nlohmann::json::parse(evt.GetString().ToStdString());
        wxString display = wxString::FromUTF8(payload.value("display", ""));
        chat_panel_->on_tool_result(display);
    } catch (...) {}
}

void LocusFrame::on_agent_turn_complete(wxThreadEvent& /*evt*/)
{
    SetStatusText("Ready", 0);
    if (tray_) tray_->set_state(LocusTray::State::idle);

    // Ensure approval panel is hidden.
    if (approval_panel_->IsShown()) {
        approval_panel_->dismiss();
        aui_.GetPane("approval").Hide();
        aui_.Update();
    }

    chat_panel_->on_turn_complete();
}

void LocusFrame::on_agent_context_meter(wxThreadEvent& evt)
{
    int used  = evt.GetInt();
    int limit = static_cast<int>(evt.GetExtraLong());
    SetStatusText(wxString::Format("ctx: %d/%d", used, limit), 1);
    chat_panel_->set_context_meter(used, limit);
}

void LocusFrame::on_agent_compaction(wxThreadEvent& /*evt*/)
{
    show_compaction_dialog();
}

void LocusFrame::on_agent_session_reset(wxThreadEvent& /*evt*/)
{
    SetStatusText("Conversation reset", 0);
    chat_panel_->on_session_reset();
}

void LocusFrame::on_agent_error(wxThreadEvent& evt)
{
    wxString msg = evt.GetString();
    SetStatusText("Error: " + msg, 0);
    if (tray_) tray_->set_state(LocusTray::State::error);
    chat_panel_->on_error(msg);
    spdlog::error("Agent error (shown in UI): {}", msg.ToStdString());
}

void LocusFrame::on_agent_embedding_progress(wxThreadEvent& evt)
{
    int done = evt.GetInt();
    int total = static_cast<int>(evt.GetExtraLong());
    file_tree_panel_->set_embedding_progress(done, total);
}

} // namespace locus

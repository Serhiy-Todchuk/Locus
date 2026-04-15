#pragma once

#include "chat_panel.h"
#include "compaction_dialog.h"
#include "locus_tray.h"
#include "tool_approval_panel.h"
#include "wx_frontend.h"
#include "../agent_core.h"
#include "../workspace.h"

#include <wx/wx.h>
#include <wx/aui/aui.h>

#include <memory>

namespace locus {

// Main application window. Owns the AUI layout with three panes:
//   left:   sidebar (file tree — placeholder for S1.6)
//   center: chat panel (wxWebView + input + footer)
//   right:  detail panel (placeholder for S1.4)
//
// Also owns the system tray icon and the WxFrontend thread bridge.
class LocusFrame : public wxFrame {
public:
    LocusFrame(AgentCore& agent, Workspace& workspace);
    ~LocusFrame() override;

    LocusFrame(const LocusFrame&) = delete;
    LocusFrame& operator=(const LocusFrame&) = delete;

private:
    void create_menu_bar();
    void create_status_bar();
    void setup_aui_layout();

    // Show the compaction dialog and apply the chosen strategy.
    void show_compaction_dialog();

    // Event handlers: window lifecycle
    void on_close(wxCloseEvent& evt);
    void on_iconize(wxIconizeEvent& evt);

    // Event handlers: agent thread events (via WxFrontend)
    void on_agent_turn_start(wxThreadEvent& evt);
    void on_agent_token(wxThreadEvent& evt);
    void on_agent_tool_pending(wxThreadEvent& evt);
    void on_agent_tool_result(wxThreadEvent& evt);
    void on_agent_turn_complete(wxThreadEvent& evt);
    void on_agent_context_meter(wxThreadEvent& evt);
    void on_agent_compaction(wxThreadEvent& evt);
    void on_agent_session_reset(wxThreadEvent& evt);
    void on_agent_error(wxThreadEvent& evt);

    // Core references (not owned)
    AgentCore& agent_;
    Workspace& workspace_;

    // Owned
    wxAuiManager           aui_;
    std::unique_ptr<LocusTray>    tray_;
    std::unique_ptr<WxFrontend>   wx_frontend_;

    // Pane panels.
    wxPanel*           sidebar_panel_  = nullptr;  // placeholder for S1.6
    ChatPanel*         chat_panel_     = nullptr;  // chat UI (S1.3)
    ToolApprovalPanel* approval_panel_ = nullptr;  // tool approval (S1.4)
    wxPanel*           detail_panel_   = nullptr;  // placeholder for S1.6 details

    wxDECLARE_EVENT_TABLE();
};

} // namespace locus

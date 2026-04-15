#pragma once

#include "locus_tray.h"
#include "wx_frontend.h"
#include "../agent_core.h"
#include "../workspace.h"

#include <wx/wx.h>
#include <wx/aui/aui.h>

#include <memory>

namespace locus {

// Main application window. Owns the AUI layout with three panes:
//   left:   sidebar (file tree — placeholder for S1.6)
//   center: chat panel (placeholder for S1.3)
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

    // Placeholder panels for the 3-pane layout.
    // Actual implementations come in S1.3 / S1.4 / S1.6.
    wxPanel* sidebar_panel_  = nullptr;
    wxPanel* chat_panel_     = nullptr;
    wxPanel* detail_panel_   = nullptr;

    wxDECLARE_EVENT_TABLE();
};

} // namespace locus

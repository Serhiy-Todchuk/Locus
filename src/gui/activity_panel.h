#pragma once

#include "../activity_event.h"
#include "../frontend.h"

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/splitter.h>
#include <wx/stc/stc.h>

#include <cstdint>
#include <vector>

namespace locus {

class ILocusCore;

// Right-side panel showing the full activity log: system prompts, user
// messages, LLM responses (with real token usage), tool calls/results,
// indexer events, warnings and errors.
//
// Events arrive on the UI thread via LocusFrame forwarding (see
// EVT_AGENT_ACTIVITY). On construction the panel queries
// ILocusCore::get_activity() to catch up on any events emitted before it
// was created (notably the initial system_prompt).
class ActivityPanel : public wxPanel {
public:
    ActivityPanel(wxWindow* parent, ILocusCore& core);

    // Append a new event and refresh the list.
    void append(const ActivityEvent& event);

    // Clear all events (e.g. on session reset).
    void clear();

    // wxListCtrl virtual-mode callbacks (public so the inner
    // ActivityListCtrl subclass can reach them).
    wxString list_text_for(long item, long col) const;
    wxListItemAttr* list_attr_for(long item) const;

private:
    void on_item_selected(wxListEvent& event);

    ILocusCore&                core_;
    wxSplitterWindow*          splitter_ = nullptr;
    wxListCtrl*                list_     = nullptr;
    wxStyledTextCtrl*          detail_   = nullptr;
    std::vector<ActivityEvent> events_;

    // Per-kind row attributes for color coding (owned).
    mutable wxListItemAttr     attr_error_;
    mutable wxListItemAttr     attr_warning_;
    mutable wxListItemAttr     attr_llm_;
    mutable wxListItemAttr     attr_index_;
};

} // namespace locus

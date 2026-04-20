#include "activity_panel.h"

#include <wx/listctrl.h>
#include <wx/splitter.h>
#include <wx/sizer.h>
#include <wx/stc/stc.h>

#include <chrono>
#include <ctime>
#include <cstdio>

namespace locus {

// Virtual wxListCtrl subclass that delegates to ActivityPanel for data.
// Using a tiny subclass avoids multiple inheritance on the panel itself.
class ActivityListCtrl : public wxListCtrl {
public:
    ActivityListCtrl(wxWindow* parent, const ActivityPanel* owner)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL)
        , owner_(owner)
    {}

protected:
    wxString OnGetItemText(long item, long col) const override;
    wxListItemAttr* OnGetItemAttr(long item) const override;

private:
    const ActivityPanel* owner_;
};

// Friend-like accessors: ActivityPanel exposes helpers consumed here.
// We store them as methods on the panel and route through a const pointer.
wxString ActivityListCtrl::OnGetItemText(long item, long col) const
{
    return owner_->list_text_for(item, col);
}

wxListItemAttr* ActivityListCtrl::OnGetItemAttr(long item) const
{
    return owner_->list_attr_for(item);
}

// ---------------------------------------------------------------------------

ActivityPanel::ActivityPanel(wxWindow* parent, ILocusCore& core)
    : wxPanel(parent, wxID_ANY)
    , core_(core)
{
    splitter_ = new wxSplitterWindow(this, wxID_ANY,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxSP_LIVE_UPDATE | wxSP_3DSASH);

    auto* list = new ActivityListCtrl(splitter_, this);
    list_ = list;
    list_->InsertColumn(0, "Time",    wxLIST_FORMAT_LEFT,  70);
    list_->InsertColumn(1, "Kind",    wxLIST_FORMAT_LEFT,  105);
    list_->InsertColumn(2, "Summary", wxLIST_FORMAT_LEFT,  340);
    list_->InsertColumn(3, "Tokens",  wxLIST_FORMAT_RIGHT, 70);

    detail_ = new wxStyledTextCtrl(splitter_, wxID_ANY);
    detail_->SetReadOnly(true);
    detail_->SetMarginWidth(1, 0);
    detail_->SetWrapMode(wxSTC_WRAP_WORD);
    detail_->StyleClearAll();

    splitter_->SplitHorizontally(list_, detail_, -160);
    splitter_->SetMinimumPaneSize(60);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(splitter_, 1, wxEXPAND);
    SetSizer(sizer);

    // Color hints per kind.
    attr_error_.SetBackgroundColour(wxColour(0xFF, 0xE0, 0xE0));
    attr_warning_.SetBackgroundColour(wxColour(0xFF, 0xF4, 0xD0));
    attr_llm_.SetTextColour(wxColour(0x20, 0x40, 0x90));
    attr_index_.SetTextColour(wxColour(0x50, 0x70, 0x50));

    list_->Bind(wxEVT_LIST_ITEM_SELECTED, &ActivityPanel::on_item_selected, this);

    // Catch up on events emitted before construction (e.g. system prompt).
    for (auto& ev : core_.get_activity(0))
        events_.push_back(ev);
    list_->SetItemCount(static_cast<long>(events_.size()));
}

void ActivityPanel::append(const ActivityEvent& event)
{
    events_.push_back(event);
    list_->SetItemCount(static_cast<long>(events_.size()));
    // Auto-scroll to the newest entry.
    if (!events_.empty())
        list_->EnsureVisible(static_cast<long>(events_.size()) - 1);
    list_->Refresh();
}

void ActivityPanel::clear()
{
    events_.clear();
    list_->SetItemCount(0);
    list_->Refresh();
    detail_->SetReadOnly(false);
    detail_->ClearAll();
    detail_->SetReadOnly(true);
}

wxString ActivityPanel::list_text_for(long item, long col) const
{
    if (item < 0 || static_cast<size_t>(item) >= events_.size())
        return {};
    const auto& ev = events_[item];
    switch (col) {
    case 0: {
        auto t = std::chrono::system_clock::to_time_t(ev.timestamp);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return wxString::FromAscii(buf);
    }
    case 1: return wxString::FromAscii(to_string(ev.kind));
    case 2: return wxString::FromUTF8(ev.summary);
    case 3: {
        if (ev.tokens_delta && *ev.tokens_delta != 0) {
            int d = *ev.tokens_delta;
            return wxString::Format("%s%d", d > 0 ? "+" : "", d);
        }
        if (ev.tokens_in) return wxString::Format("%d", *ev.tokens_in);
        return {};
    }
    }
    return {};
}

wxListItemAttr* ActivityPanel::list_attr_for(long item) const
{
    if (item < 0 || static_cast<size_t>(item) >= events_.size())
        return nullptr;
    switch (events_[item].kind) {
    case ActivityKind::error:        return &attr_error_;
    case ActivityKind::warning:      return &attr_warning_;
    case ActivityKind::llm_response: return &attr_llm_;
    case ActivityKind::index_event:  return &attr_index_;
    default: return nullptr;
    }
}

void ActivityPanel::on_item_selected(wxListEvent& event)
{
    long idx = event.GetIndex();
    if (idx < 0 || static_cast<size_t>(idx) >= events_.size())
        return;
    const auto& ev = events_[idx];

    std::string text;
    text += "[" + std::string(to_string(ev.kind)) + "]  " + ev.summary + "\n";
    if (ev.tokens_in || ev.tokens_out || ev.tokens_delta) {
        text += "tokens: ";
        if (ev.tokens_in)    text += "total=" + std::to_string(*ev.tokens_in) + " ";
        if (ev.tokens_out)   text += "out="   + std::to_string(*ev.tokens_out) + " ";
        if (ev.tokens_delta) text += "delta=" + std::to_string(*ev.tokens_delta);
        text += "\n";
    }
    text += std::string(60, '-');
    text += "\n";
    text += ev.detail;

    detail_->SetReadOnly(false);
    detail_->ClearAll();
    detail_->AddTextRaw(text.c_str());
    detail_->SetReadOnly(true);
}

} // namespace locus

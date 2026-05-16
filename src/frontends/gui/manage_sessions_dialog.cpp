#include "manage_sessions_dialog.h"

#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

#include <chrono>
#include <sstream>

namespace locus {

namespace {
    enum {
        ID_BTN_OPEN    = wxID_HIGHEST + 700,
        ID_BTN_RENAME,
        ID_BTN_DELETE,
        ID_BTN_CLOSE,
    };
}

ManageSessionsDialog::ManageSessionsDialog(wxWindow* parent,
                                           std::vector<SessionInfo> sessions,
                                           std::unordered_set<std::string> open_ids,
                                           std::vector<std::string> preselect_ids,
                                           Mode mode,
                                           Hooks hooks)
    : wxDialog(parent, wxID_ANY, "Manage Sessions",
               wxDefaultPosition, wxSize(780, 540),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      mode_(mode),
      hooks_(std::move(hooks)),
      sessions_(std::move(sessions)),
      open_ids_(std::move(open_ids))
{
    for (auto& id : preselect_ids) preselect_.insert(std::move(id));

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    if (mode_ == Mode::cleanup) {
        heading_ = new wxStaticText(this, wxID_ANY,
            "These sessions match your auto-cleanup criteria. Confirm "
            "deletion or adjust selection. Currently-open tabs are exempt.");
        heading_->Wrap(720);
        sizer->Add(heading_, 0, wxALL | wxEXPAND, 12);
    } else {
        heading_ = new wxStaticText(this, wxID_ANY,
            "Select sessions to operate on. Currently-open tabs cannot be "
            "deleted from here -- close them first via the tab right-click "
            "menu.");
        heading_->Wrap(720);
        sizer->Add(heading_, 0, wxALL | wxEXPAND, 12);
    }

    list_ = new wxListView(this, wxID_ANY,
                            wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_HRULES);
    list_->AppendColumn("Title",     wxLIST_FORMAT_LEFT,  280);
    list_->AppendColumn("Messages",  wxLIST_FORMAT_RIGHT,  80);
    list_->AppendColumn("Last opened", wxLIST_FORMAT_LEFT, 140);
    list_->AppendColumn("Size",      wxLIST_FORMAT_RIGHT, 100);
    list_->AppendColumn("Status",    wxLIST_FORMAT_LEFT,   90);
    sizer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    populate();

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->Add(new wxButton(this, ID_BTN_OPEN,   "Open in New Tab"), 0, wxRIGHT, 6);
    btns->Add(new wxButton(this, ID_BTN_RENAME, "Rename..."),       0, wxRIGHT, 6);
    btns->Add(new wxButton(this, ID_BTN_DELETE, "Delete Selected..."), 0);
    btns->AddStretchSpacer(1);
    btns->Add(new wxButton(this, ID_BTN_CLOSE,  "Close"),           0);
    sizer->Add(btns, 0, wxALL | wxEXPAND, 12);

    SetSizer(sizer);

    Bind(wxEVT_BUTTON, &ManageSessionsDialog::on_open,   this, ID_BTN_OPEN);
    Bind(wxEVT_BUTTON, &ManageSessionsDialog::on_rename, this, ID_BTN_RENAME);
    Bind(wxEVT_BUTTON, &ManageSessionsDialog::on_delete, this, ID_BTN_DELETE);
    Bind(wxEVT_BUTTON, &ManageSessionsDialog::on_close,  this, ID_BTN_CLOSE);
}

void ManageSessionsDialog::populate()
{
    list_->DeleteAllItems();
    for (size_t i = 0; i < sessions_.size(); ++i) {
        const auto& s = sessions_[i];
        long row = list_->InsertItem(static_cast<long>(i),
                                      wxString::FromUTF8(s.title.empty() ? s.id : s.title));
        list_->SetItem(row, 1, wxString::Format("%d", s.message_count));
        list_->SetItem(row, 2, format_age(s.last_opened_at));
        list_->SetItem(row, 3, format_size(s.size_bytes));
        bool is_open = open_ids_.count(s.id) > 0;
        list_->SetItem(row, 4, is_open ? "Open" : "Closed");

        if (mode_ == Mode::cleanup && preselect_.count(s.id) > 0 && !is_open) {
            list_->Select(row, true);
        }
        if (is_open) {
            // Dim the row visually -- wxListView doesn't have a "disabled"
            // state, but we can colour it.
            wxListItem li;
            li.SetId(row);
            li.SetTextColour(wxColour(140, 140, 140));
            list_->SetItem(li);
        }
    }
}

std::vector<int> ManageSessionsDialog::selected_indices() const
{
    std::vector<int> out;
    long sel = -1;
    while ((sel = list_->GetNextSelected(sel)) != -1)
        out.push_back(static_cast<int>(sel));
    return out;
}

void ManageSessionsDialog::on_open(wxCommandEvent& /*evt*/)
{
    auto sel = selected_indices();
    if (sel.empty()) return;
    if (!hooks_.on_open) return;

    // Only open the first selection (multi-open is gratuitous click-bait).
    const auto& s = sessions_[sel.front()];
    hooks_.on_open(s.id);
    EndModal(wxID_OK);
}

void ManageSessionsDialog::on_rename(wxCommandEvent& /*evt*/)
{
    auto sel = selected_indices();
    if (sel.empty()) return;
    if (!hooks_.on_rename) return;

    const auto& s = sessions_[sel.front()];
    auto new_title = hooks_.on_rename(s.id, s.title);
    if (!new_title.empty()) {
        sessions_[sel.front()].title = new_title;
        list_->SetItem(sel.front(), 0, wxString::FromUTF8(new_title));
    }
}

void ManageSessionsDialog::on_delete(wxCommandEvent& /*evt*/)
{
    auto sel = selected_indices();
    if (sel.empty()) return;

    // Filter out currently-open sessions so the confirm reflects the actual
    // delete set.
    std::vector<int> deletable;
    for (int i : sel) {
        if (open_ids_.count(sessions_[i].id) == 0) deletable.push_back(i);
    }
    if (deletable.empty()) {
        wxMessageBox("Selected sessions are currently open in tabs. "
                     "Close them via the tab right-click menu first.",
                     "Manage Sessions", wxOK | wxICON_INFORMATION, this);
        return;
    }

    wxString msg = wxString::Format(
        "Permanently delete %zu session(s) and any archived history? "
        "This cannot be undone.", deletable.size());
    int answer = wxMessageBox(msg, "Delete Sessions",
                              wxYES_NO | wxICON_WARNING, this);
    if (answer != wxYES) return;

    if (hooks_.on_delete) {
        // Erase highest-index first so subsequent indices stay valid.
        std::sort(deletable.begin(), deletable.end(), std::greater<int>());
        for (int i : deletable) {
            hooks_.on_delete(sessions_[i].id);
            sessions_.erase(sessions_.begin() + i);
        }
        populate();
    }
}

void ManageSessionsDialog::on_close(wxCommandEvent& /*evt*/)
{
    EndModal(wxID_OK);
}

wxString ManageSessionsDialog::format_age(long long last_opened_at)
{
    if (last_opened_at <= 0) return "(unknown)";
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long diff = now - last_opened_at;
    if (diff < 0) diff = 0;
    if (diff < 60)        return wxString::Format("%llds ago", diff);
    if (diff < 3600)      return wxString::Format("%lldm ago", diff / 60);
    if (diff < 86400)     return wxString::Format("%lldh ago", diff / 3600);
    if (diff < 86400 * 30) return wxString::Format("%lldd ago", diff / 86400);
    return wxString::Format("%lldmo ago", diff / (86400 * 30));
}

wxString ManageSessionsDialog::format_size(long long bytes)
{
    if (bytes < 1024) return wxString::Format("%lld B", bytes);
    if (bytes < 1024 * 1024)
        return wxString::Format("%.1f KB", bytes / 1024.0);
    return wxString::Format("%.2f MB", bytes / (1024.0 * 1024.0));
}

} // namespace locus

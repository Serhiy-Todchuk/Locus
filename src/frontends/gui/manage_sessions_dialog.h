#pragma once

#include "../../agent/session_manager.h"

#include <wx/wx.h>
#include <wx/listctrl.h>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace locus {

// S5.I -- one-stop browse/manage UI for `.locus/sessions/`. Two entry modes:
//   normal: show every session, no rows pre-selected
//   cleanup: pre-filter to candidates that match the auto-cleanup criteria
//            (last_opened_at > delete_after_days AND past keep_last_count),
//            pre-select them, and surface a heading explaining what's about
//            to be deleted. Used both by the Session menu's "Clean Up Old
//            Sessions..." entry and by the workspace-open auto-cleanup hook.
//
// Currently-open tabs appear in the list with status = "Open" and are not
// selectable for deletion (use the tab right-click "Delete session..."
// instead, which closes the tab first). Deleting a session removes BOTH the
// `<id>.json` file AND the `<id>/` archive directory in one operation -- see
// the S5.I doc's on-disk layout note.
class ManageSessionsDialog : public wxDialog {
public:
    enum class Mode { normal, cleanup };

    // Callbacks (supplied by the caller -- LocusFrame). All run on the UI thread.
    struct Hooks {
        // Open a saved session into a new tab. Closes the dialog on success.
        std::function<void(const std::string& session_id)> on_open;
        // Rename a session's title. Returns the new title or empty on cancel.
        std::function<std::string(const std::string& session_id,
                                   const std::string& current_title)> on_rename;
        // Remove a session's on-disk artifacts (JSON + archive dir).
        std::function<void(const std::string& session_id)> on_delete;
    };

    ManageSessionsDialog(wxWindow* parent,
                         std::vector<SessionInfo> sessions,
                         std::unordered_set<std::string> open_ids,
                         std::vector<std::string> preselect_ids,
                         Mode mode,
                         Hooks hooks);

private:
    void populate();
    void on_open(wxCommandEvent& evt);
    void on_rename(wxCommandEvent& evt);
    void on_delete(wxCommandEvent& evt);
    void on_close(wxCommandEvent& evt);
    std::vector<int> selected_indices() const;

    static wxString format_age(long long last_opened_at);
    static wxString format_size(long long bytes);

    Mode mode_;
    Hooks hooks_;
    std::vector<SessionInfo>           sessions_;
    std::unordered_set<std::string>    open_ids_;
    std::unordered_set<std::string>    preselect_;

    wxListView*  list_      = nullptr;
    wxStaticText* heading_  = nullptr;
};

} // namespace locus

#pragma once

#include <wx/wx.h>
#include <wx/popupwin.h>
#include <wx/listbox.h>

#include <functional>
#include <string>
#include <vector>

namespace locus {

// Autocomplete dropdown shown under the chat input when the user types `@`
// followed by file-path characters. Mirrors `SlashPopup`'s API so ChatPanel
// can drive both with the same key-handler shape.
//
// The candidate list is the workspace-relative path list captured once per
// session (cheap: we already have it from `IndexQuery::list_directory`).
// Filtering is substring + prefix-preferred over the basename, with the
// full path used as a tie-breaker so `chat_panel` ranks above
// `src/foo/chat_panel.cpp` even when both share the prefix.
class MentionPopup : public wxPopupTransientWindow {
public:
    MentionPopup(wxWindow* parent, std::vector<std::string> file_paths);

    // Filter by substring of the basename (preferred) or full path. Returns
    // true if at least one item is visible.
    bool apply_filter(const wxString& filter);

    // Show anchored at the input widget. The popup is placed above the
    // anchor since the input sits at the bottom of the main frame.
    void show_anchored(const wxPoint& anchor_screen_pos, int anchor_width);

    void move_up();
    void move_down();

    // Workspace-relative path of the currently highlighted entry; empty
    // string when nothing is highlighted (or the list is empty).
    std::string selected_path() const;
    bool        has_selection() const;

    std::function<void(const std::string&)> on_accept;
    std::function<void()>                   on_dismiss;

protected:
    void OnDismiss() override;

private:
    void rebuild();
    void on_list_left_up(wxMouseEvent& evt);
    void on_list_motion(wxMouseEvent& evt);

    std::vector<std::string> all_paths_;
    std::vector<size_t>      visible_;  // indices into all_paths_
    wxListBox*               list_ = nullptr;
};

} // namespace locus

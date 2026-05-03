#pragma once

#include <wx/wx.h>
#include <wx/popupwin.h>
#include <wx/listbox.h>

#include <functional>
#include <string>
#include <vector>

namespace locus {

// A single entry shown in the slash suggestion dropdown.
struct SlashItem {
    std::string name;         // e.g. "reset" or "read_file" (no leading '/')
    std::string description;  // short hint shown after the name
    bool        is_tool = false;
};

// Autocomplete dropdown shown under the chat input when the user types '/'.
// Owns a wxListBox; items are filtered by substring match on the name.
// Accept is signalled via on_accept; dismissal (click outside) via on_dismiss.
class SlashPopup : public wxPopupTransientWindow {
public:
    SlashPopup(wxWindow* parent, std::vector<SlashItem> items);

    // Filter by substring. Returns true if at least one item is visible.
    bool apply_filter(const wxString& filter);

    // Show anchored below the given widget (screen coords).
    void show_anchored(const wxPoint& anchor_screen_pos, int anchor_width);

    // Selection navigation.
    void move_up();
    void move_down();

    // Selected command name (without leading '/'); empty if none.
    std::string selected_command() const;
    bool        has_selection() const;

    // Callbacks.
    std::function<void(const std::string&)> on_accept;   // item chosen
    std::function<void()>                   on_dismiss;  // popup closed by click-outside

protected:
    void OnDismiss() override;

private:
    void rebuild();
    void on_list_left_up(wxMouseEvent& evt);
    void on_list_motion(wxMouseEvent& evt);

    std::vector<SlashItem> all_items_;
    std::vector<size_t>    visible_;  // indices into all_items_
    wxListBox*             list_ = nullptr;
};

} // namespace locus

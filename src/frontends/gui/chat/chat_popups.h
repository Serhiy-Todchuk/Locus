#pragma once

#include "../mention_popup.h"
#include "../slash_popup.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wx/textctrl.h>
#include <wx/wx.h>

namespace locus {

// Manages the slash-command and @-mention autocomplete popups for ChatPanel.
//
// Both popups are anchored to the input widget and share the same keyboard nav
// contract (Esc/Up/Down/Tab/Enter). ChatPanel calls handle_key() from its
// on_input_key handler; update() from on_input_text; dismiss_all() on submit.
// The input widget pointer is kept for text reading (active token detection)
// and for inserting accepted suggestions via ChangeValue + SetFocus.
class ChatPopups {
public:
    // `parent` is the ChatPanel window (popup parent for z-ordering).
    // `input` is the text input widget used for text reading and caret position.
    ChatPopups(wxWindow* parent, wxTextCtrl* input);

    // Seed the slash popup candidate list (rebuilds the popup lazily on next '/').
    void set_slash_commands(std::vector<SlashItem> items);

    // Seed the @-mention candidate list.
    void set_mention_paths(std::vector<std::string> paths);

    // Read-only access to mention paths (used by submit logic in ChatPanel).
    const std::vector<std::string>& mention_paths() const { return mention_paths_; }

    // Call from ChatPanel::on_input_key. Returns true if the key was consumed
    // by a popup navigation action (caller should NOT call evt.Skip()).
    bool handle_key(wxKeyEvent& evt);

    // Call from ChatPanel::on_input_text to show/hide/filter both popups.
    void update();

    // Hide both popups (called on submit and on focus loss).
    void dismiss_all();

    bool slash_visible()   const;
    bool mention_visible() const;

    // Active mention detection used by ChatPanel::submit_current_input for the
    // @<path> text-replacement on accept.
    struct ActiveMention {
        size_t   start = std::string::npos;
        wxString prefix;
    };
    ActiveMention active_mention_at_cursor() const;

private:
    wxString active_slash_token() const;

    void update_slash();
    void update_mention();
    void hide_slash();
    void hide_mention();
    void accept_slash(const std::string& name);
    void accept_mention(const std::string& path);

    wxWindow*   parent_;
    wxTextCtrl* input_;

    std::vector<SlashItem>       slash_commands_;
    std::unique_ptr<SlashPopup>  slash_popup_;
    bool                         slash_popup_shown_ = false;

    std::vector<std::string>      mention_paths_;
    std::unique_ptr<MentionPopup> mention_popup_;
    bool                          mention_popup_shown_ = false;
};

} // namespace locus

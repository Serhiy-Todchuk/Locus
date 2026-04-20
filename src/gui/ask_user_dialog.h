#pragma once

#include <wx/wx.h>

#include <string>

namespace locus {

// Modal popup for the ask_user tool. Shows the question, a multi-line text
// input, and Reply/Cancel buttons. Disappears as soon as the user answers or
// cancels.
//
// Keyboard: Enter=Reply (Shift+Enter inserts newline), Esc=Cancel.
class AskUserDialog : public wxDialog {
public:
    AskUserDialog(wxWindow* parent, const std::string& question);

    // Valid after ShowModal() returns wxID_OK.
    std::string response() const { return response_; }

private:
    void on_reply(wxCommandEvent& evt);
    void on_cancel(wxCommandEvent& evt);
    void on_key(wxKeyEvent& evt);

    wxTextCtrl* input_ = nullptr;
    std::string response_;
};

} // namespace locus

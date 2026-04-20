#include "tool_approval_panel.h"
#include "theme.h"

#include <spdlog/spdlog.h>

namespace locus {

enum {
    ID_BTN_APPROVE = wxID_HIGHEST + 300,
    ID_BTN_REJECT,
    ID_BTN_MODIFY,
};

ToolApprovalPanel::ToolApprovalPanel(wxWindow* parent,
                                     ToolDecisionCallback on_decision)
    : wxPanel(parent, wxID_ANY)
    , on_decision_(std::move(on_decision))
{
    // Match the OS window colour so the panel blends with the dark (or light) frame.
    SetBackgroundColour(theme::panel_bg());
    create_controls();

    main_sizer_ = new wxBoxSizer(wxVERTICAL);

    // Tool name + preview (always visible when shown).
    main_sizer_->Add(name_label_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    main_sizer_->Add(preview_label_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    // JSON args display.
    main_sizer_->Add(args_stc_, 1, wxEXPAND | wxALL, 8);

    // Ask-user input (hidden by default).
    main_sizer_->Add(ask_input_, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    // Button row.
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->Add(btn_approve_, 0, wxRIGHT, 4);
    btn_sizer->Add(btn_modify_, 0, wxRIGHT, 4);
    btn_sizer->Add(btn_reject_, 0);
    main_sizer_->Add(btn_sizer, 0, wxALIGN_RIGHT | wxALL, 8);

    SetSizer(main_sizer_);

    // Start hidden.
    Hide();

    // Global key handler for this panel.
    Bind(wxEVT_CHAR_HOOK, &ToolApprovalPanel::on_key, this);
}

void ToolApprovalPanel::create_controls()
{
    // Tool name badge.
    name_label_ = new wxStaticText(this, wxID_ANY, "");
    auto font = name_label_->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    font.SetPointSize(font.GetPointSize() + 1);
    name_label_->SetFont(font);

    // Preview text.
    preview_label_ = new wxStaticText(this, wxID_ANY, "");
    preview_label_->SetForegroundColour(theme::muted_fg());

    // JSON args — Scintilla with JSON lexer.
    args_stc_ = new wxStyledTextCtrl(this, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 150));
    args_stc_->SetLexer(wxSTC_LEX_JSON);
    args_stc_->SetReadOnly(true);
    args_stc_->SetWrapMode(wxSTC_WRAP_WORD);
    args_stc_->SetMarginWidth(0, 0);  // no line numbers
    args_stc_->SetMarginWidth(1, 0);  // no symbols margin

    // Apply a consistent background + default foreground to every style, then
    // override per-token foregrounds. Order matters: StyleClearAll() copies
    // STYLE_DEFAULT to all styles, so it has to come before the per-style sets.
    const bool dark = theme::is_dark();
    const wxColour bg = theme::text_bg();
    const wxColour fg = theme::text_fg();
    args_stc_->StyleSetBackground(wxSTC_STYLE_DEFAULT, bg);
    args_stc_->StyleSetForeground(wxSTC_STYLE_DEFAULT, fg);
    args_stc_->StyleClearAll();
    args_stc_->SetCaretForeground(theme::caret_fg());
    args_stc_->SetSelBackground(true, theme::selection_bg());

    // JSON syntax colours — two palettes so tokens read well on either theme.
    const wxColour c_number   = dark ? wxColour(181, 206, 168) : wxColour(0, 128, 128);
    const wxColour c_string   = dark ? wxColour(214, 157, 133) : wxColour(163, 21, 21);
    const wxColour c_property = dark ? wxColour(156, 220, 254) : wxColour(0, 0, 180);
    const wxColour c_keyword  = dark ? wxColour(86, 156, 214)  : wxColour(0, 0, 180);
    const wxColour c_comment  = dark ? wxColour(87, 166, 74)   : wxColour(0, 128, 0);

    args_stc_->StyleSetForeground(wxSTC_JSON_DEFAULT,      fg);
    args_stc_->StyleSetForeground(wxSTC_JSON_NUMBER,       c_number);
    args_stc_->StyleSetForeground(wxSTC_JSON_STRING,       c_string);
    args_stc_->StyleSetForeground(wxSTC_JSON_PROPERTYNAME, c_property);
    args_stc_->StyleSetForeground(wxSTC_JSON_KEYWORD,      c_keyword);
    args_stc_->StyleSetForeground(wxSTC_JSON_OPERATOR,     fg);
    args_stc_->StyleSetForeground(wxSTC_JSON_BLOCKCOMMENT, c_comment);
    args_stc_->StyleSetForeground(wxSTC_JSON_LINECOMMENT,  c_comment);

    // Monospace font for all JSON styles.
    wxFont mono(10, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL,
                false, "Cascadia Code");
    if (!mono.IsOk())
        mono = wxFont(10, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    for (int i = 0; i <= wxSTC_JSON_ERROR; ++i)
        args_stc_->StyleSetFont(i, mono);

    // Ask-user text input (hidden by default).
    ask_input_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, 60),
        wxTE_MULTILINE | wxTE_PROCESS_ENTER);
    ask_input_->SetHint("Type your response...");
    ask_input_->SetBackgroundColour(theme::text_bg());
    ask_input_->SetForegroundColour(theme::text_fg());
    ask_input_->Hide();

    // Buttons.
    btn_approve_ = new wxButton(this, ID_BTN_APPROVE, "Approve (Enter)");
    btn_reject_  = new wxButton(this, ID_BTN_REJECT,  "Reject (Esc)");
    btn_modify_  = new wxButton(this, ID_BTN_MODIFY,  "Modify (M)");

    btn_approve_->SetBackgroundColour(wxColour(76, 175, 80));
    btn_approve_->SetForegroundColour(*wxWHITE);
    btn_reject_->SetBackgroundColour(wxColour(244, 67, 54));
    btn_reject_->SetForegroundColour(*wxWHITE);

    btn_approve_->Bind(wxEVT_BUTTON, &ToolApprovalPanel::on_approve, this);
    btn_reject_->Bind(wxEVT_BUTTON, &ToolApprovalPanel::on_reject, this);
    btn_modify_->Bind(wxEVT_BUTTON, &ToolApprovalPanel::on_modify, this);
}

void ToolApprovalPanel::show_tool_call(const std::string& call_id,
                                       const std::string& tool_name,
                                       const nlohmann::json& args,
                                       const std::string& preview)
{
    call_id_       = call_id;
    tool_name_     = tool_name;
    original_args_ = args;
    editing_       = false;

    if (tool_name == "ask_user") {
        layout_ask_user();
    } else {
        layout_normal();
    }

    name_label_->SetLabel(wxString::FromUTF8(tool_name));
    if (!preview.empty())
        preview_label_->SetLabel(wxString::FromUTF8(preview));
    else
        preview_label_->SetLabel("");

    Show();
    GetParent()->Layout();

    // Focus the right control.
    if (tool_name == "ask_user")
        ask_input_->SetFocus();
    else
        btn_approve_->SetFocus();
}

void ToolApprovalPanel::dismiss()
{
    Hide();
    GetParent()->Layout();
}

void ToolApprovalPanel::layout_normal()
{
    // Show: args STC, all three buttons. Hide: ask_input.
    args_stc_->Show();
    args_stc_->SetReadOnly(false);
    args_stc_->SetText(original_args_.dump(2));
    args_stc_->SetReadOnly(true);
    ask_input_->Hide();

    btn_approve_->SetLabel("Approve (Enter)");
    btn_modify_->Show();
}

void ToolApprovalPanel::layout_ask_user()
{
    // Show: ask_input. Hide: args STC, modify button.
    args_stc_->Hide();
    ask_input_->Show();
    ask_input_->Clear();

    btn_approve_->SetLabel("Reply (Enter)");
    btn_modify_->Hide();

    // Show the question as the preview.
    std::string question = original_args_.value("question", "");
    if (!question.empty())
        preview_label_->SetLabel(wxString::FromUTF8(question));
}

// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------

void ToolApprovalPanel::on_approve(wxCommandEvent& /*evt*/)
{
    if (tool_name_ == "ask_user") {
        // Pack the user's response into modified args.
        std::string response = ask_input_->GetValue().ToStdString(wxConvUTF8);
        nlohmann::json modified = original_args_;
        modified["response"] = response;
        if (on_decision_)
            on_decision_(call_id_, ToolDecision::modify, modified);
    } else if (editing_) {
        // Parse edited JSON.
        try {
            auto modified = nlohmann::json::parse(args_stc_->GetText().ToStdString());
            if (on_decision_)
                on_decision_(call_id_, ToolDecision::modify, modified);
        } catch (const nlohmann::json::parse_error& e) {
            wxMessageBox(wxString::Format("Invalid JSON: %s", e.what()),
                         "Parse Error", wxOK | wxICON_WARNING, this);
            return;  // don't dismiss
        }
    } else {
        if (on_decision_)
            on_decision_(call_id_, ToolDecision::approve, {});
    }
    dismiss();
}

void ToolApprovalPanel::on_reject(wxCommandEvent& /*evt*/)
{
    send_decision(ToolDecision::reject);
}

void ToolApprovalPanel::on_modify(wxCommandEvent& /*evt*/)
{
    if (tool_name_ == "ask_user") return;  // no modify for ask_user

    editing_ = !editing_;
    args_stc_->SetReadOnly(!editing_);

    if (editing_) {
        btn_modify_->SetLabel("Cancel Edit");
        btn_approve_->SetLabel("Confirm (Enter)");
        args_stc_->SetFocus();
    } else {
        btn_modify_->SetLabel("Modify (M)");
        btn_approve_->SetLabel("Approve (Enter)");
        // Revert to original.
        args_stc_->SetReadOnly(false);
        args_stc_->SetText(original_args_.dump(2));
        args_stc_->SetReadOnly(true);
    }
}

void ToolApprovalPanel::on_key(wxKeyEvent& evt)
{
    int key = evt.GetKeyCode();

    // Don't intercept keys when typing in the ask_user input or editing args.
    if (ask_input_->HasFocus() || (editing_ && args_stc_->HasFocus())) {
        if (key == WXK_RETURN && !evt.ShiftDown() && ask_input_->HasFocus()) {
            wxCommandEvent dummy;
            on_approve(dummy);
            return;
        }
        if (key == WXK_ESCAPE) {
            send_decision(ToolDecision::reject);
            return;
        }
        evt.Skip();
        return;
    }

    switch (key) {
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER: {
        wxCommandEvent dummy;
        on_approve(dummy);
        break;
    }
    case WXK_ESCAPE:
        send_decision(ToolDecision::reject);
        break;
    case 'M':
    case 'm': {
        wxCommandEvent dummy;
        on_modify(dummy);
        break;
    }
    default:
        evt.Skip();
        break;
    }
}

void ToolApprovalPanel::send_decision(ToolDecision decision)
{
    if (on_decision_)
        on_decision_(call_id_, decision, {});
    dismiss();
}

} // namespace locus

#include "tool_approval_panel.h"
#include "theme.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

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
    ask_input_->Hide();

    // S4.A: edit_file / multi_edit_file render as a colored unified diff
    // instead of raw JSON, so the user can approve at a glance. Modify mode
    // flips back to JSON via on_modify() → render_json_view().
    if (!render_diff_view())
        render_json_view();

    btn_approve_->SetLabel("Approve (Enter)");
    btn_modify_->Show();
}

// -- Diff / JSON rendering inside args_stc_ ---------------------------------

// Custom Scintilla style slots. Must sit above the JSON lexer's range
// (wxSTC_JSON_ERROR is ~12) and below STYLE_DEFAULT (32). Pick 40+ to steer
// well clear; Scintilla allows up to 255 custom styles.
enum : int {
    kStyleDiffContext = 40,
    kStyleDiffAdd     = 41,
    kStyleDiffDel     = 42,
    kStyleDiffHeader  = 43,
};

bool ToolApprovalPanel::render_diff_view()
{
    const bool is_edit = (tool_name_ == "edit_file" ||
                          tool_name_ == "multi_edit_file");
    if (!is_edit) return false;

    // Gather edits into a flat list. edit_file has a single edit embedded in
    // the top-level args; multi_edit_file carries them in `edits[]`.
    struct EditPair { std::string old_s; std::string new_s; bool all; };
    std::vector<EditPair> edits;

    std::string path = original_args_.value("path", "");

    if (tool_name_ == "edit_file") {
        edits.push_back({original_args_.value("old_string", ""),
                         original_args_.value("new_string", ""),
                         original_args_.value("replace_all", false)});
    } else {
        if (original_args_.contains("edits") &&
            original_args_["edits"].is_array()) {
            for (const auto& e : original_args_["edits"]) {
                edits.push_back({e.value("old_string", ""),
                                 e.value("new_string", ""),
                                 e.value("replace_all", false)});
            }
        }
    }

    // Switch lexer off so only our hand-applied styling drives colour.
    args_stc_->SetReadOnly(false);
    args_stc_->SetLexer(wxSTC_LEX_NULL);
    args_stc_->ClearAll();

    // Rebuild the per-line styling so repeated renders don't accumulate.
    const bool dark = theme::is_dark();
    args_stc_->StyleSetBackground(kStyleDiffContext, theme::text_bg());
    args_stc_->StyleSetForeground(kStyleDiffContext, theme::text_fg());
    args_stc_->StyleSetBackground(kStyleDiffAdd,
        dark ? wxColour(30, 80, 40) : wxColour(220, 255, 220));
    args_stc_->StyleSetForeground(kStyleDiffAdd,
        dark ? wxColour(200, 255, 200) : wxColour(20, 90, 20));
    args_stc_->StyleSetBackground(kStyleDiffDel,
        dark ? wxColour(90, 30, 30) : wxColour(255, 220, 220));
    args_stc_->StyleSetForeground(kStyleDiffDel,
        dark ? wxColour(255, 200, 200) : wxColour(120, 20, 20));
    args_stc_->StyleSetBackground(kStyleDiffHeader, theme::panel_bg());
    args_stc_->StyleSetForeground(kStyleDiffHeader, theme::muted_fg());
    args_stc_->StyleSetBold(kStyleDiffHeader, true);

    wxFont mono(10, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL,
                wxFONTWEIGHT_NORMAL, false, "Cascadia Code");
    if (!mono.IsOk())
        mono = wxFont(10, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL,
                      wxFONTWEIGHT_NORMAL);
    args_stc_->StyleSetFont(kStyleDiffContext, mono);
    args_stc_->StyleSetFont(kStyleDiffAdd,     mono);
    args_stc_->StyleSetFont(kStyleDiffDel,     mono);
    args_stc_->StyleSetFont(kStyleDiffHeader,  mono);

    // Assemble the display text and a parallel style-per-byte vector, then
    // push them in one shot. Scintilla's SetStyling writes len bytes of style
    // starting at the current position set by StartStyling.
    std::string        text;
    std::vector<char>  styles;
    auto push_line = [&](const std::string& line, int style) {
        text.append(line);
        text.push_back('\n');
        styles.insert(styles.end(), line.size() + 1, static_cast<char>(style));
    };

    int idx = 0;
    for (const auto& e : edits) {
        std::string header = "@@ edit " + std::to_string(++idx) + "/" +
                             std::to_string(edits.size()) + " — " + path;
        if (e.all) header += "  [replace_all]";
        push_line(header, kStyleDiffHeader);

        // Walk each string line-by-line so newline-bearing snippets look like
        // a real diff instead of a single wall of red/green.
        auto emit_block = [&](const std::string& s, char sign, int style) {
            size_t start = 0;
            for (size_t i = 0; i <= s.size(); ++i) {
                if (i == s.size() || s[i] == '\n') {
                    std::string line(1, sign);
                    line.append(s, start, i - start);
                    push_line(line, style);
                    start = i + 1;
                    if (i == s.size()) break;
                }
            }
        };
        emit_block(e.old_s, '-', kStyleDiffDel);
        emit_block(e.new_s, '+', kStyleDiffAdd);
        if (idx < static_cast<int>(edits.size()))
            push_line("", kStyleDiffContext);
    }

    args_stc_->SetText(wxString::FromUTF8(text.c_str()));
    args_stc_->StartStyling(0);
    // Walk contiguous runs of the same style — SetStyling applies one style
    // over N bytes, so we issue one call per run.
    int run_start = 0;
    for (int i = 1; i <= static_cast<int>(styles.size()); ++i) {
        if (i == static_cast<int>(styles.size()) || styles[i] != styles[run_start]) {
            args_stc_->SetStyling(i - run_start, styles[run_start]);
            run_start = i;
        }
    }

    args_stc_->SetReadOnly(true);
    return true;
}

void ToolApprovalPanel::render_json_view()
{
    // Restore the JSON lexer + the pretty-printed args.
    args_stc_->SetReadOnly(false);
    args_stc_->SetLexer(wxSTC_LEX_JSON);
    args_stc_->SetText(original_args_.dump(2));
    args_stc_->SetReadOnly(true);
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

    if (editing_) {
        // Entering edit mode — always show JSON so the user can tweak args.
        render_json_view();
        args_stc_->SetReadOnly(false);
        btn_modify_->SetLabel("Cancel Edit");
        btn_approve_->SetLabel("Confirm (Enter)");
        args_stc_->SetFocus();
    } else {
        btn_modify_->SetLabel("Modify (M)");
        btn_approve_->SetLabel("Approve (Enter)");
        // Leaving edit mode — go back to the preferred display: diff for edit
        // tools, JSON for everything else.
        if (!render_diff_view())
            render_json_view();
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

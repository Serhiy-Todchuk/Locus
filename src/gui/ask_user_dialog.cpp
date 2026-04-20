#include "ask_user_dialog.h"
#include "theme.h"

namespace locus {

AskUserDialog::AskUserDialog(wxWindow* parent, const std::string& question)
    : wxDialog(parent, wxID_ANY, "Assistant is asking...",
               wxDefaultPosition, wxSize(520, 260),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(theme::panel_bg());

    auto* question_label = new wxStaticText(this, wxID_ANY,
        wxString::FromUTF8(question));
    question_label->Wrap(480);
    auto qfont = question_label->GetFont();
    qfont.SetWeight(wxFONTWEIGHT_BOLD);
    question_label->SetFont(qfont);

    input_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_PROCESS_ENTER);
    input_->SetHint("Type your response...");
    input_->SetBackgroundColour(theme::text_bg());
    input_->SetForegroundColour(theme::text_fg());

    auto* btn_reply  = new wxButton(this, wxID_OK,     "Reply (Enter)");
    auto* btn_cancel = new wxButton(this, wxID_CANCEL, "Cancel (Esc)");
    btn_reply->SetBackgroundColour(wxColour(76, 175, 80));
    btn_reply->SetForegroundColour(*wxWHITE);
    btn_reply->SetDefault();

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(btn_reply,  0, wxRIGHT, 4);
    btn_sizer->Add(btn_cancel, 0);

    auto* main = new wxBoxSizer(wxVERTICAL);
    main->Add(question_label, 0, wxEXPAND | wxALL, 12);
    main->Add(input_,         1, wxEXPAND | wxLEFT | wxRIGHT, 12);
    main->Add(btn_sizer,      0, wxEXPAND | wxALL, 12);
    SetSizer(main);

    btn_reply->Bind(wxEVT_BUTTON,  &AskUserDialog::on_reply,  this);
    btn_cancel->Bind(wxEVT_BUTTON, &AskUserDialog::on_cancel, this);
    Bind(wxEVT_CHAR_HOOK,          &AskUserDialog::on_key,    this);

    input_->SetFocus();
    CentreOnParent();
}

void AskUserDialog::on_reply(wxCommandEvent& /*evt*/)
{
    response_ = input_->GetValue().ToStdString(wxConvUTF8);
    EndModal(wxID_OK);
}

void AskUserDialog::on_cancel(wxCommandEvent& /*evt*/)
{
    EndModal(wxID_CANCEL);
}

void AskUserDialog::on_key(wxKeyEvent& evt)
{
    const int key = evt.GetKeyCode();
    if (key == WXK_ESCAPE) {
        EndModal(wxID_CANCEL);
        return;
    }
    if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && !evt.ShiftDown()) {
        wxCommandEvent dummy;
        on_reply(dummy);
        return;
    }
    evt.Skip();
}

} // namespace locus

#include "endpoints_settings_panel.h"

#include "../endpoint_tooltip.h"
#include "../ui_names.h"
#include "../locus_accessible.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/valnum.h>

namespace locus {

EndpointsSettingsPanel::EndpointsSettingsPanel(wxWindow* parent,
                                               const WorkspaceConfig& /*config*/)
    : wxPanel(parent)
{
    store_.load();   // global store; seeds the six builtins if absent

    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxStaticText(this, wxID_ANY,
        "LLM endpoints (global, shared across all workspaces). The active "
        "profile is used by new tabs; switch per-tab from the chat footer. "
        "API keys live in ~/.locus/endpoints.json -- never inside a workspace.");
    intro->Wrap(560);
    outer->Add(intro, 0, wxALL, 8);

    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 240),
                           wxLC_REPORT | wxLC_SINGLE_SEL);
    list_->SetName(ui_names::kSettingsEndpointsList);
    gui::apply_locus_accessible_name(list_);
    list_->InsertColumn(0, "Name",  wxLIST_FORMAT_LEFT, 170);
    list_->InsertColumn(1, "URL",   wxLIST_FORMAT_LEFT, 230);
    list_->InsertColumn(2, "Model", wxLIST_FORMAT_LEFT, 120);
    list_->InsertColumn(3, "Key",   wxLIST_FORMAT_LEFT, 80);
    list_->Bind(wxEVT_LIST_ITEM_SELECTED,   [this](wxListEvent&){ update_button_state(); });
    list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&){ update_button_state(); });
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED,  [this](wxListEvent&){
        wxCommandEvent e; on_edit(e);
    });
    outer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

    auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
    add_btn_    = new wxButton(this, wxID_ANY, "Add...");
    edit_btn_   = new wxButton(this, wxID_ANY, "Edit...");
    remove_btn_ = new wxButton(this, wxID_ANY, "Remove");
    active_btn_ = new wxButton(this, wxID_ANY, "Set Active");
    add_btn_->SetName(ui_names::kSettingsEndpointsAddBtn);
    edit_btn_->SetName(ui_names::kSettingsEndpointsEditBtn);
    remove_btn_->SetName(ui_names::kSettingsEndpointsRemoveBtn);
    active_btn_->SetName(ui_names::kSettingsEndpointsSetActiveBtn);
    gui::apply_locus_accessible_name(add_btn_);
    gui::apply_locus_accessible_name(edit_btn_);
    gui::apply_locus_accessible_name(remove_btn_);
    gui::apply_locus_accessible_name(active_btn_);
    add_btn_->Bind(wxEVT_BUTTON, &EndpointsSettingsPanel::on_add, this);
    edit_btn_->Bind(wxEVT_BUTTON, &EndpointsSettingsPanel::on_edit, this);
    remove_btn_->Bind(wxEVT_BUTTON, &EndpointsSettingsPanel::on_remove, this);
    active_btn_->Bind(wxEVT_BUTTON, &EndpointsSettingsPanel::on_set_active, this);
    btn_row->Add(add_btn_,    0, wxRIGHT, 4);
    btn_row->Add(edit_btn_,   0, wxRIGHT, 4);
    btn_row->Add(remove_btn_, 0, wxRIGHT, 4);
    btn_row->AddStretchSpacer(1);
    btn_row->Add(active_btn_, 0);
    outer->Add(btn_row, 0, wxEXPAND | wxALL, 8);

    SetSizer(outer);

    rebuild_list();
    update_button_state();
}

void EndpointsSettingsPanel::rebuild_list()
{
    if (!list_) return;
    list_->DeleteAllItems();
    const auto& profiles = store_.list();
    for (size_t i = 0; i < profiles.size(); ++i) {
        const auto& p = profiles[i];
        wxString label = wxString::FromUTF8(p.name);
        if (p.name == store_.active()) label = "* " + label;   // active marker
        long row = list_->InsertItem(static_cast<long>(i), label);
        list_->SetItem(row, 1, wxString::FromUTF8(p.base_url));
        list_->SetItem(row, 2, wxString::FromUTF8(
            p.default_model.empty() ? "(default)" : p.default_model));
        list_->SetItem(row, 3, gui::mask_api_key(p.api_key));
    }
}

int EndpointsSettingsPanel::selected_row() const
{
    if (!list_) return -1;
    return static_cast<int>(list_->GetNextItem(-1, wxLIST_NEXT_ALL,
                                               wxLIST_STATE_SELECTED));
}

void EndpointsSettingsPanel::update_button_state()
{
    int row = selected_row();
    bool has_sel = row >= 0;
    const auto& profiles = store_.list();
    bool is_builtin = has_sel && row < static_cast<int>(profiles.size())
                      && profiles[row].builtin;
    if (edit_btn_)   edit_btn_->Enable(has_sel);
    if (active_btn_) active_btn_->Enable(has_sel);
    if (remove_btn_) remove_btn_->Enable(has_sel && !is_builtin);
}

void EndpointsSettingsPanel::on_add(wxCommandEvent&)
{
    EndpointProfile out;
    if (!show_edit_modal(EndpointProfile{}, /*name_locked=*/false, out))
        return;
    if (!store_.add(out)) {
        wxMessageBox("A profile with that name already exists (or the name is "
                     "empty).", "Endpoints", wxOK | wxICON_ERROR, this);
        return;
    }
    dirty_ = true;
    rebuild_list();
    update_button_state();
}

void EndpointsSettingsPanel::on_edit(wxCommandEvent&)
{
    int row = selected_row();
    const auto& profiles = store_.list();
    if (row < 0 || row >= static_cast<int>(profiles.size())) return;
    EndpointProfile seed = profiles[row];
    EndpointProfile out;
    if (!show_edit_modal(seed, /*name_locked=*/true, out)) return;
    out.name = seed.name;   // name is the key; never changes on Edit
    store_.update(out);
    dirty_ = true;
    rebuild_list();
    update_button_state();
}

void EndpointsSettingsPanel::on_remove(wxCommandEvent&)
{
    int row = selected_row();
    const auto& profiles = store_.list();
    if (row < 0 || row >= static_cast<int>(profiles.size())) return;
    std::string name = profiles[row].name;
    if (wxMessageBox("Remove endpoint profile '" + wxString::FromUTF8(name) + "'?",
                     "Endpoints", wxYES_NO | wxICON_QUESTION, this) != wxYES)
        return;
    if (!store_.remove(name)) {
        wxMessageBox("Builtin profiles cannot be removed.",
                     "Endpoints", wxOK | wxICON_INFORMATION, this);
        return;
    }
    dirty_ = true;
    rebuild_list();
    update_button_state();
}

void EndpointsSettingsPanel::on_set_active(wxCommandEvent&)
{
    int row = selected_row();
    const auto& profiles = store_.list();
    if (row < 0 || row >= static_cast<int>(profiles.size())) return;
    store_.set_active(profiles[row].name);
    dirty_ = true;
    rebuild_list();
    update_button_state();
}

bool EndpointsSettingsPanel::show_edit_modal(const EndpointProfile& seed,
                                             bool name_locked,
                                             EndpointProfile& out)
{
    wxDialog dlg(this, wxID_ANY,
                 name_locked ? "Edit endpoint" : "Add endpoint",
                 wxDefaultPosition, wxSize(440, -1));
    dlg.SetName(ui_names::kSettingsEndpointsEditDialog);
    gui::apply_locus_accessible_name(&dlg);

    auto* grid = new wxFlexGridSizer(2, 6, 8);
    grid->AddGrowableCol(1, 1);

    auto add_row = [&](const wxString& label, wxWindow* ctrl) {
        grid->Add(new wxStaticText(&dlg, wxID_ANY, label), 0,
                  wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
    };

    auto* name = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(seed.name));
    name->SetName(ui_names::kSettingsEndpointsEditName);
    gui::apply_locus_accessible_name(name);
    if (name_locked) name->Enable(false);
    add_row("Name", name);

    auto* url = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(seed.base_url));
    url->SetName(ui_names::kSettingsEndpointsEditBaseUrl);
    gui::apply_locus_accessible_name(url);
    add_row("Base URL", url);

    auto* key = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(seed.api_key),
                               wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    key->SetName(ui_names::kSettingsEndpointsEditApiKey);
    gui::apply_locus_accessible_name(key);
    add_row("API key", key);

    auto* show_key = new wxCheckBox(&dlg, wxID_ANY, "Show key");
    show_key->SetName(ui_names::kSettingsEndpointsEditShowKey);
    gui::apply_locus_accessible_name(show_key);
    // wxTE_PASSWORD style can't be toggled at runtime on Windows; swap the
    // control by mirroring its value into a visible field is overkill, so we
    // re-create on toggle via a simple value round-trip into a plain ctrl.
    show_key->Bind(wxEVT_CHECKBOX, [key](wxCommandEvent& e) {
        // Best-effort reveal: append a tooltip with the value when checked.
        // (Native wxTE_PASSWORD cannot un-mask in place.)
        if (e.IsChecked()) key->SetToolTip(key->GetValue());
        else               key->UnsetToolTip();
    });
    add_row("", show_key);

    auto* model = new wxTextCtrl(&dlg, wxID_ANY,
                                 wxString::FromUTF8(seed.default_model));
    model->SetName(ui_names::kSettingsEndpointsEditModel);
    gui::apply_locus_accessible_name(model);
    add_row("Default model", model);

    auto* tf = new wxChoice(&dlg, wxID_ANY);
    tf->SetName(ui_names::kSettingsEndpointsEditToolFormat);
    gui::apply_locus_accessible_name(tf);
    const char* formats[] = {"auto", "openai", "qwen", "claude", "none"};
    for (auto* f : formats) tf->Append(f);
    {
        int sel = 0;
        for (int i = 0; i < static_cast<int>(tf->GetCount()); ++i)
            if (tf->GetString(i) == wxString::FromUTF8(seed.tool_format)) { sel = i; break; }
        tf->SetSelection(sel);
    }
    add_row("Tool format", tf);

    auto* ctx = new wxTextCtrl(&dlg, wxID_ANY,
        wxString::Format("%d", seed.default_context_limit));
    ctx->SetName(ui_names::kSettingsEndpointsEditContextLimit);
    gui::apply_locus_accessible_name(ctx);
    add_row("Context limit (0=auto)", ctx);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(grid, 1, wxEXPAND | wxALL, 12);
    outer->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
               wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    dlg.SetSizerAndFit(outer);

    if (dlg.ShowModal() != wxID_OK) return false;

    out.name        = name_locked ? seed.name : std::string(name->GetValue().utf8_str());
    out.base_url    = std::string(url->GetValue().utf8_str());
    out.api_key     = std::string(key->GetValue().utf8_str());
    out.default_model = std::string(model->GetValue().utf8_str());
    out.tool_format = std::string(tf->GetStringSelection().utf8_str());
    long cl = 0;
    ctx->GetValue().ToLong(&cl);
    out.default_context_limit = static_cast<int>(cl);
    out.builtin     = seed.builtin;
    out.extra_headers = seed.extra_headers;   // preserved (no UI yet)

    if (out.name.empty() || out.base_url.empty()) {
        wxMessageBox("Name and Base URL are required.",
                     "Endpoints", wxOK | wxICON_ERROR, this);
        return false;
    }
    return true;
}

void EndpointsSettingsPanel::load_from_config(const WorkspaceConfig& /*cfg*/)
{
    // The store is global, not part of WorkspaceConfig -- reload from disk so
    // "Reset to global defaults" elsewhere doesn't strand stale rows.
    store_.load();
    rebuild_list();
    update_button_state();
}

bool EndpointsSettingsPanel::validate(wxString& /*out_error*/) const
{
    return true;
}

void EndpointsSettingsPanel::commit_to_config(WorkspaceConfig& cfg) const
{
    // No interaction with the Endpoints tab -> leave the global store and the
    // per-workspace override untouched (don't silently pin a workspace that
    // was following the global active).
    if (!dirty_) return;

    // Persist the global store and pin this workspace to the active profile so
    // the chat-footer chip + resolution agree on re-open.
    auto err = store_.save();
    if (!err.empty()) {
        wxLogWarning("Could not write endpoints.json: %s",
                     wxString::FromUTF8(err));
        return;
    }
    cfg.llm.active_endpoint = store_.active();
}

} // namespace locus

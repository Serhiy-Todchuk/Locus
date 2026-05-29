#pragma once

#include "settings_panel.h"
#include "../../../llm/endpoint_profile.h"

#include <wx/wx.h>
#include <wx/listctrl.h>

namespace locus {

// S6.16 -- Settings > Endpoints tab. Manages the GLOBAL endpoints.json store
// (add / edit / remove / set-active), distinct from the per-workspace knobs
// the other panels edit. The list shows Name | URL | Model | Key (masked).
// commit_to_config persists the store and pins this workspace to the active
// profile via cfg.llm.active_endpoint. `dirty()` tells the dialog whether to
// re-broadcast the endpoint to open chat tabs after OK.
class EndpointsSettingsPanel : public wxPanel, public ISettingsPanel {
public:
    EndpointsSettingsPanel(wxWindow* parent, const WorkspaceConfig& config);

    void load_from_config(const WorkspaceConfig& cfg) override;
    bool validate(wxString& out_error) const override;
    void commit_to_config(WorkspaceConfig& cfg) const override;

    // True when the user added / edited / removed / set-active any profile.
    bool dirty() const { return dirty_; }

    // The store's active profile name (after any edits in this session).
    std::string active_profile() const { return store_.active(); }

private:
    void rebuild_list();
    void update_button_state();
    int  selected_row() const;

    void on_add(wxCommandEvent&);
    void on_edit(wxCommandEvent&);
    void on_remove(wxCommandEvent&);
    void on_set_active(wxCommandEvent&);

    // Returns true + fills `out` when the user accepts the edit modal.
    // `seed` pre-fills the fields (empty for Add). `name_locked` greys the
    // name field (Edit can't rename -- the name is the store key).
    bool show_edit_modal(const EndpointProfile& seed, bool name_locked,
                         EndpointProfile& out);

    EndpointProfileStore store_;
    wxListCtrl*          list_       = nullptr;
    wxButton*            add_btn_    = nullptr;
    wxButton*            edit_btn_   = nullptr;
    wxButton*            remove_btn_ = nullptr;
    wxButton*            active_btn_ = nullptr;
    bool                 dirty_      = false;
};

} // namespace locus

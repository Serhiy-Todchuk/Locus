#pragma once

#include "../../core/workspace.h"

#include <wx/wx.h>
#include <wx/checkbox.h>

namespace locus {

// S5.A first-open / Settings modal for the five workspace capability buckets.
// Reads defaults from the WorkspaceConfig::Capabilities passed in; writes the
// user's chosen values back on OK. Cancel leaves the struct untouched.
//
// Used both as the first-time-open prompt (shown by LocusApp before the
// frame opens, when `.locus/config.json` is missing) and via Settings ->
// Capabilities for later changes.
class CapabilitiesDialog : public wxDialog {
public:
    CapabilitiesDialog(wxWindow* parent, WorkspaceConfig::Capabilities& caps);

    // True if the user clicked OK and any checkbox actually changed.
    bool changed() const { return changed_; }

private:
    void on_ok(wxCommandEvent& evt);
    void build_row(wxSizer* parent, wxCheckBox*& ctrl,
                   const wxString& label, int token_estimate,
                   const wxString& tooltip, bool initial);

    WorkspaceConfig::Capabilities& caps_;
    WorkspaceConfig::Capabilities  initial_;
    bool         changed_ = false;

    wxCheckBox*  cb_bg_      = nullptr;
    wxCheckBox*  cb_semantic_ = nullptr;
    wxCheckBox*  cb_code_    = nullptr;
    wxCheckBox*  cb_memory_  = nullptr;
    wxCheckBox*  cb_web_     = nullptr;
};

} // namespace locus

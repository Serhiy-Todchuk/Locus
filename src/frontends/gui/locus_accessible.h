#pragma once

// S5.L -- minimal wxAccessible that surfaces `wxWindow::SetName()` as the
// accessibility Name (which Windows MSAA / UIA then exposes as the Name
// property).
//
// wxWidgets defaults to `wxWindowAccessible` for windows that didn't call
// `SetAccessible()`. That default reads the name from `GetWindowText()`,
// which is empty for `wxPanel` and just the visible label for buttons --
// neither stable nor unique. To find a control reliably from the UIA
// driver in tests/ui_automation/, we want the name we passed to
// `SetName(ui_names::kFoo)`. This shim makes that happen.
//
// Usage:
//   widget->SetName(ui_names::kFoo);
//   apply_locus_accessible_name(widget);
//
// The window takes ownership of the wxAccessible (wxWindow::SetAccessible
// deletes any previous one and frees the new one in ~wxWindow).
//
// `wxUSE_ACCESSIBILITY=1` must be on in the wxWidgets build (verified for
// the current vcpkg build). The whole file compiles to nothing otherwise.

#include <wx/wx.h>

#if wxUSE_ACCESSIBILITY
#include <wx/access.h>
#endif

namespace locus::gui {

#if wxUSE_ACCESSIBILITY

class LocusAccessible : public wxAccessible {
public:
    explicit LocusAccessible(wxWindow* win) : wxAccessible(win) {}

    // Report `m_window->GetName()` so the UIA Name property reflects the
    // SetName() call. Everything else falls back to wxAccessible's defaults.
    wxAccStatus GetName(int childId, wxString* name) override
    {
        if (!name || childId != 0 || !GetWindow()) return wxACC_FAIL;
        *name = GetWindow()->GetName();
        if (name->empty()) return wxACC_NOT_IMPLEMENTED;
        return wxACC_OK;
    }

    // For wxButton (and subclasses) also expose the visible label via UIA
    // Value, so the agentic harness can read state without losing the
    // accessible-name override that find_named relies on. Same path now
    // covers wxStaticText (and its subclass wxGenericHyperlinkCtrl) so the
    // chat-footer chips (ctx_label / plan_chip / commit_chip / etc.) return
    // their RENDERED text instead of the automation_id literal -- closes
    // TestLocalVibe finding H3.
    //
    // wxControl::GetLabelText covers the (very rare) other label-bearing
    // wx widgets too; fall back to GetLabel() for anything that doesn't
    // implement GetLabelText cleanly.
    wxAccStatus GetValue(int childId, wxString* value) override
    {
        if (!value || childId != 0 || !GetWindow()) return wxACC_FAIL;
        if (auto* btn = wxDynamicCast(GetWindow(), wxButton)) {
            *value = btn->GetLabelText();
            return value->empty() ? wxACC_NOT_IMPLEMENTED : wxACC_OK;
        }
        if (auto* st = wxDynamicCast(GetWindow(), wxStaticText)) {
            *value = st->GetLabelText();
            return value->empty() ? wxACC_NOT_IMPLEMENTED : wxACC_OK;
        }
        return wxACC_NOT_IMPLEMENTED;
    }
};

inline void apply_locus_accessible_name(wxWindow* win)
{
    if (!win) return;
    win->SetAccessible(new LocusAccessible(win));
}

#else  // !wxUSE_ACCESSIBILITY

inline void apply_locus_accessible_name(wxWindow*) {}

#endif

} // namespace locus::gui

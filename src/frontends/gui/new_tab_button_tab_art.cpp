#include "new_tab_button_tab_art.h"

#include <wx/aui/auibook.h>
#include <wx/panel.h>

#include <memory>

namespace locus {

wxWindow* install_new_tab_button(wxAuiNotebook* notebook,
                                 std::function<void()> on_click)
{
    if (!notebook) return nullptr;

    auto* placeholder = new wxPanel(notebook);
    placeholder->Hide();
    notebook->AddPage(placeholder, "+");
    notebook->SetPageToolTip(notebook->GetPageCount() - 1,
                             "New tab (Ctrl+T)");

    auto cb = std::make_shared<std::function<void()>>(std::move(on_click));

    notebook->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGING,
                   [notebook, placeholder, cb](wxAuiNotebookEvent& evt) {
        int target = evt.GetSelection();
        if (target < 0) { evt.Skip(); return; }
        if (notebook->GetPage(target) != placeholder) { evt.Skip(); return; }
        // Veto the switch and fire the new-tab callback. The callback is
        // expected to create a real tab and select it.
        evt.Veto();
        if (cb && *cb) (*cb)();
    });

    return placeholder;
}

} // namespace locus

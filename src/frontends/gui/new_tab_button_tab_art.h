#pragma once

#include <wx/aui/auibook.h>
#include <wx/window.h>

#include <functional>

namespace locus {

// Adds a non-closable "+" placeholder tab at the end of the notebook that
// fires on_click when the user tries to activate it. The placeholder is
// vetoed from ever becoming the active tab, which keeps wxAuiNotebook from
// ever painting a close button on it (with the default
// wxAUI_NB_CLOSE_ON_ACTIVE_TAB style) or showing its empty page widget.
//
// Returns the placeholder wxWindow*. The caller is expected to:
//   - keep this pointer
//   - skip it when iterating the notebook's pages by index
//   - InsertPage(GetPageCount() - 1, ...) when adding real tabs so the
//     placeholder stays rightmost.
//
// Caller must install the placeholder AFTER the initial real tabs have
// been added (otherwise the placeholder ends up between/at-start of the
// real pages).
wxWindow* install_new_tab_button(wxAuiNotebook* notebook,
                                 std::function<void()> on_click);

} // namespace locus

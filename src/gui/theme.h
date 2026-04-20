#pragma once

#include <wx/settings.h>
#include <wx/colour.h>

namespace locus::theme {

inline bool is_dark()
{
    return wxSystemSettings::GetAppearance().IsDark();
}

// Editable text control background (wxTextCtrl / Scintilla edit area).
inline wxColour text_bg()
{
    return is_dark() ? wxColour(30, 30, 30) : wxColour(255, 255, 255);
}

inline wxColour text_fg()
{
    return is_dark() ? wxColour(220, 220, 220) : wxColour(0, 0, 0);
}

// Panel background — follow the OS "window" colour so panels blend with the frame.
inline wxColour panel_bg()
{
    return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
}

// Secondary / hint text colour — readable on either theme.
inline wxColour muted_fg()
{
    return wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
}

// Caret colour (inside edit controls).
inline wxColour caret_fg()
{
    return is_dark() ? wxColour(220, 220, 220) : wxColour(0, 0, 0);
}

// Selection background inside edit controls.
inline wxColour selection_bg()
{
    return is_dark() ? wxColour(60, 90, 140) : wxColour(173, 214, 255);
}

} // namespace locus::theme

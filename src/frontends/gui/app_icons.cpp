#include "app_icons.h"

#include <wx/artprov.h>
#include <wx/string.h>

namespace locus::gui {
namespace {

wxSize icon_size(int size)
{
    const int resolved = size > 0 ? size : 32;
    return wxSize(resolved, resolved);
}

wxIcon load_resource_icon(const char* name, int size)
{
    wxIcon icon;
#if defined(__WXMSW__)
    icon.LoadFile(wxString::FromAscii(name),
                  wxBITMAP_TYPE_ICO_RESOURCE,
                  size,
                  size);
#else
    (void)name;
    (void)size;
#endif
    return icon;
}

wxIcon fallback_icon(int size)
{
    return wxArtProvider::GetIcon(wxART_EXECUTABLE_FILE,
                                  wxART_OTHER,
                                  icon_size(size));
}

wxIcon resource_or_fallback(const char* name, int size)
{
    wxIcon icon = load_resource_icon(name, size);
    if (icon.IsOk()) return icon;
    return fallback_icon(size);
}

} // namespace

wxIcon app_icon(int size)
{
    return resource_or_fallback("locus_app", size);
}

wxIcon tray_idle_icon()
{
    return resource_or_fallback("locus_tray_idle", 16);
}

wxIcon tray_active_icon()
{
    return resource_or_fallback("locus_tray_active", 16);
}

wxIcon tray_indexing_icon()
{
    return resource_or_fallback("locus_tray_indexing", 16);
}

wxIcon tray_error_icon()
{
    return resource_or_fallback("locus_tray_error", 16);
}

} // namespace locus::gui

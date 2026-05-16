#pragma once

#include <wx/icon.h>

namespace locus::gui {

wxIcon app_icon(int size = 32);
wxIcon tray_idle_icon();
wxIcon tray_active_icon();
wxIcon tray_indexing_icon();
wxIcon tray_error_icon();

} // namespace locus::gui

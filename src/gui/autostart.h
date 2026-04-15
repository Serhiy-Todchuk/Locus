#pragma once

#include <string>

namespace locus {

// Windows startup-on-login via HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
// Opt-in, off by default. Writes/removes the "Locus" value pointing to the
// current executable path.

bool is_autostart_enabled();
void set_autostart_enabled(bool enable);

} // namespace locus

#include "autostart.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace locus {

static const char* k_run_key  = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* k_val_name = "Locus";

bool is_autostart_enabled()
{
#ifdef _WIN32
    HKEY hkey = nullptr;
    LONG rc = RegOpenKeyExA(HKEY_CURRENT_USER, k_run_key, 0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    rc = RegQueryValueExA(hkey, k_val_name, nullptr, &type, nullptr, nullptr);
    RegCloseKey(hkey);
    return rc == ERROR_SUCCESS && type == REG_SZ;
#else
    return false;
#endif
}

void set_autostart_enabled(bool enable)
{
#ifdef _WIN32
    HKEY hkey = nullptr;
    LONG rc = RegOpenKeyExA(HKEY_CURRENT_USER, k_run_key, 0, KEY_SET_VALUE, &hkey);
    if (rc != ERROR_SUCCESS) {
        spdlog::warn("Autostart: cannot open Run key: {}", rc);
        return;
    }

    if (enable) {
        // Get the current executable path.
        char path[MAX_PATH] = {};
        DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            spdlog::warn("Autostart: GetModuleFileName failed");
            RegCloseKey(hkey);
            return;
        }

        rc = RegSetValueExA(hkey, k_val_name, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(path),
                            static_cast<DWORD>(strlen(path) + 1));
        if (rc == ERROR_SUCCESS)
            spdlog::info("Autostart: enabled ({})", path);
        else
            spdlog::warn("Autostart: RegSetValueEx failed: {}", rc);
    } else {
        rc = RegDeleteValueA(hkey, k_val_name);
        if (rc == ERROR_SUCCESS)
            spdlog::info("Autostart: disabled");
        else if (rc != ERROR_FILE_NOT_FOUND)
            spdlog::warn("Autostart: RegDeleteValue failed: {}", rc);
    }

    RegCloseKey(hkey);
#else
    (void)enable;
    spdlog::warn("Autostart: not implemented on this platform");
#endif
}

} // namespace locus

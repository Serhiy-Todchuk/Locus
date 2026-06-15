#include "autostart.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <mach-o/dyld.h>
#endif

namespace locus {

static const char* k_run_key  = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* k_val_name = "Locus";

#if defined(__APPLE__)
namespace {

namespace fs = std::filesystem;

// ~/Library/LaunchAgents/com.locus.Locus.plist -- a per-user LaunchAgent with
// RunAtLoad loads at each login while the file is present, so the toggle is
// just "file exists or not". (Takes effect at next login; we don't bootstrap
// it into the running session.)
constexpr const char* k_plist_label = "com.locus.Locus";

fs::path launch_agent_plist_path()
{
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return fs::path(home) / "Library" / "LaunchAgents" /
           (std::string(k_plist_label) + ".plist");
}

// Absolute path to this running executable (locus_gui).
std::string self_executable_path()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);   // queries the needed buffer size
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::path(buf.data()), ec);
    return ec ? std::string(buf.data()) : resolved.string();
}

} // namespace
#endif

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
#elif defined(__APPLE__)
    fs::path p = launch_agent_plist_path();
    std::error_code ec;
    return !p.empty() && fs::exists(p, ec);
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
#elif defined(__APPLE__)
    fs::path plist = launch_agent_plist_path();
    if (plist.empty()) {
        spdlog::warn("Autostart: $HOME unset; cannot resolve LaunchAgents path");
        return;
    }
    std::error_code ec;
    if (enable) {
        std::string exe = self_executable_path();
        if (exe.empty()) {
            spdlog::warn("Autostart: could not resolve own executable path");
            return;
        }
        fs::create_directories(plist.parent_path(), ec);
        std::ofstream out(plist, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::warn("Autostart: cannot write {}", plist.string());
            return;
        }
        out <<
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "    <key>Label</key>\n"
            "    <string>" << k_plist_label << "</string>\n"
            "    <key>ProgramArguments</key>\n"
            "    <array>\n"
            "        <string>" << exe << "</string>\n"
            "    </array>\n"
            "    <key>RunAtLoad</key>\n"
            "    <true/>\n"
            "    <key>ProcessType</key>\n"
            "    <string>Interactive</string>\n"
            "    <key>LimitLoadToSessionType</key>\n"
            "    <string>Aqua</string>\n"
            "</dict>\n"
            "</plist>\n";
        out.close();
        spdlog::info("Autostart: enabled ({})", plist.string());
    } else {
        if (fs::remove(plist, ec))
            spdlog::info("Autostart: disabled");
        else if (ec)
            spdlog::warn("Autostart: cannot remove {}: {}", plist.string(), ec.message());
    }
#else
    (void)enable;
    spdlog::warn("Autostart: not implemented on this platform");
#endif
}

} // namespace locus

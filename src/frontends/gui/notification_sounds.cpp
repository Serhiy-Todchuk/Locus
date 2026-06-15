#include "notification_sounds.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <mmsystem.h>
#elif defined(__APPLE__)
#  include <AudioToolbox/AudioToolbox.h>
#  include <CoreFoundation/CoreFoundation.h>
#endif

namespace locus::notification_sounds {

namespace {

bool enabled_for(Kind kind, const WorkspaceConfig::Notifications& n)
{
    switch (kind) {
        case Kind::tool_approval: return n.sound_on_tool_approval;
        case Kind::ask_user:      return n.sound_on_ask_user;
        case Kind::turn_complete: return n.sound_on_turn_complete;
        case Kind::compaction:    return n.sound_on_compaction;
    }
    return false;
}

#ifdef _WIN32
const wchar_t* alias_for(Kind kind)
{
    switch (kind) {
        case Kind::tool_approval: return L"SystemExclamation";
        case Kind::ask_user:      return L"SystemQuestion";
        case Kind::turn_complete: return L"SystemAsterisk";
        case Kind::compaction:    return L"SystemHand";
    }
    return L"SystemDefault";
}
#elif defined(__APPLE__)
const char* sound_file_for(Kind kind)
{
    switch (kind) {
        case Kind::tool_approval: return "/System/Library/Sounds/Tink.aiff";
        case Kind::ask_user:      return "/System/Library/Sounds/Glass.aiff";
        case Kind::turn_complete: return "/System/Library/Sounds/Pop.aiff";
        case Kind::compaction:    return "/System/Library/Sounds/Sosumi.aiff";
    }
    return "/System/Library/Sounds/Tink.aiff";
}

// Lazily register the four built-in sounds as SystemSoundIDs and cache them
// for the app's lifetime (AudioServices wants the ID disposed eventually, but
// four IDs living until exit is fine -- they map to small built-in clips).
SystemSoundID system_sound_id(Kind kind)
{
    static SystemSoundID ids[4] = {0, 0, 0, 0};
    int idx = static_cast<int>(kind);
    if (idx < 0 || idx > 3) idx = 0;
    if (ids[idx] == 0) {
        CFStringRef path = CFStringCreateWithCString(
            kCFAllocatorDefault, sound_file_for(kind), kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(
            kCFAllocatorDefault, path, kCFURLPOSIXPathStyle, false);
        AudioServicesCreateSystemSoundID(url, &ids[idx]);
        CFRelease(url);
        CFRelease(path);
    }
    return ids[idx];
}
#endif

} // namespace

void play(Kind kind, const WorkspaceConfig& cfg, const wxFrame* frame)
{
    const auto& n = cfg.notifications;
    if (!enabled_for(kind, n)) return;

#ifdef _WIN32
    if (n.only_when_unfocused && frame) {
        // Treat both the frame itself and any of its child/owned windows
        // (modal dialogs etc.) being foreground as "focused".
        HWND fg  = ::GetForegroundWindow();
        HWND own = frame->GetHWND() ? static_cast<HWND>(frame->GetHWND()) : nullptr;
        if (fg && own && (fg == own || ::GetAncestor(fg, GA_ROOTOWNER) == own)) {
            return;
        }
    }

    ::PlaySoundW(alias_for(kind), nullptr,
                 SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
#elif defined(__APPLE__)
    // App-level "active" is the right granularity on macOS (mirrors
    // NSApplication.isActive); a modal we own keeps the app active too.
    (void)frame;
    if (n.only_when_unfocused && wxTheApp && wxTheApp->IsActive())
        return;

    SystemSoundID sid = system_sound_id(kind);
    if (sid != 0) AudioServicesPlaySystemSound(sid);  // non-blocking
#else
    (void)frame;
#endif
}

} // namespace locus::notification_sounds

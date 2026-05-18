#include "notification_sounds.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <mmsystem.h>
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
#else
    (void)frame;
#endif
}

} // namespace locus::notification_sounds

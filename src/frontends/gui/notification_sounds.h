#pragma once

#include "../../core/workspace.h"

#include <wx/wx.h>

namespace locus {

// Per-event sound alerts for moments that need the user's attention.
//
// Windows: PlaySound with SystemX aliases (waveforms follow the user's
//   Control Panel -> Sounds settings):
//     tool_approval -> SystemExclamation   ask_user   -> SystemQuestion
//     turn_complete -> SystemAsterisk       compaction -> SystemHand
// macOS: AudioServices on the built-in /System/Library/Sounds/*.aiff
//   (C API, non-blocking, no Objective-C):
//     tool_approval -> Tink   ask_user   -> Glass
//     turn_complete -> Pop    compaction -> Sosumi
namespace notification_sounds {

enum class Kind {
    tool_approval,
    ask_user,
    turn_complete,
    compaction
};

// Play the sound for `kind` if the corresponding flag in `cfg.notifications`
// is true. If `cfg.notifications.only_when_unfocused` is true and `frame` is
// the foreground window, the call is a no-op. Always non-blocking
// (PlaySound is called with SND_ASYNC). Safe to call from the UI thread;
// PlaySound is not safe from worker threads so callers must marshal first.
void play(Kind kind, const WorkspaceConfig& cfg, const wxFrame* frame);

} // namespace notification_sounds
} // namespace locus

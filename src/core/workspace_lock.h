#pragma once

#include <filesystem>

namespace locus {

// Process-wide exclusive lock on a workspace folder.
//
// Prevents two Locus processes (CLI or GUI, in any combination) from opening
// the same workspace at the same time. Also refuses to open a workspace
// whose ancestor is already open as a Locus workspace — the common "oops I
// opened the parent project *and* a subdir of it" mistake.
//
// Mechanism: exclusive file lock on `<root>/.locus/locus.lock`. The OS
// releases the lock when the process exits, so crashes don't leave stale
// locks. The lock file also carries diagnostic JSON (PID, start time,
// canonical path) — on conflict, the error message quotes it so the user
// knows which instance to close.
//
// Scope limitation: descendant detection (parent opened after a child is
// already open) is not yet implemented — walking a large tree on every
// startup costs too much. If this becomes a real-world issue, we can add
// a shared registry file in %LOCALAPPDATA% / $XDG_STATE_HOME.
class WorkspaceLock {
public:
    // Acquire the lock. Creates `.locus/` if needed. Throws
    // `std::runtime_error` with a human-readable message when:
    //   - another process already holds this workspace's lock
    //   - an ancestor folder is already open as a workspace
    //   - the lock file can't be created (permissions, etc.)
    explicit WorkspaceLock(const std::filesystem::path& workspace_root);
    ~WorkspaceLock();

    WorkspaceLock(const WorkspaceLock&) = delete;
    WorkspaceLock& operator=(const WorkspaceLock&) = delete;

    const std::filesystem::path& lock_path() const { return lock_path_; }

private:
    std::filesystem::path lock_path_;

#ifdef _WIN32
    void* handle_ = nullptr;  // Win32 HANDLE, kept opaque to avoid <windows.h> in the header
#else
    int   fd_ = -1;
#endif
};

} // namespace locus

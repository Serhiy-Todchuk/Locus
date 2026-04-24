#include "core/workspace_lock.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace locus {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Best-effort read of the lock file's diagnostic JSON. Returns "" on any
// error — the caller folds whatever comes back into the error message.
std::string read_lock_diagnostic(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string raw = ss.str();
    if (raw.empty()) return {};
    try {
        auto j = json::parse(raw);
        std::string out;
        if (j.contains("pid"))     out += "PID "  + std::to_string(j["pid"].get<long long>());
        if (j.contains("started")) out += (out.empty() ? "" : ", ") +
                                         std::string("started ") + j["started"].get<std::string>();
        return out;
    } catch (...) {
        return {};
    }
}

std::string now_iso8601()
{
    using namespace std::chrono;
    auto t  = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

#ifdef _WIN32

// Try to acquire an exclusive non-blocking lock on an ancestor's lock file.
// Returns true if the ancestor is *not* currently locked (we probed + released).
// Returns false if the ancestor's process still holds it.
bool ancestor_is_stale(const fs::path& lock_path)
{
    std::wstring w = lock_path.wstring();
    HANDLE h = CreateFileW(w.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return true;  // file not openable → treat as stale

    OVERLAPPED ov{};
    BOOL ok = LockFileEx(h,
                         LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                         0, MAXDWORD, MAXDWORD, &ov);
    if (ok) {
        UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
        CloseHandle(h);
        return true;
    }
    CloseHandle(h);
    return false;
}

#else

bool ancestor_is_stale(const fs::path& lock_path)
{
    int fd = ::open(lock_path.c_str(), O_RDWR);
    if (fd < 0) return true;
    int rc = ::flock(fd, LOCK_EX | LOCK_NB);
    if (rc == 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return true;
    }
    ::close(fd);
    return false;
}

#endif

// Walk ancestors of `root` looking for a still-held `.locus/locus.lock`.
// Throws on the first conflict found.
void check_ancestors(const fs::path& root)
{
    fs::path cur = root.parent_path();
    while (!cur.empty() && cur != cur.parent_path()) {
        fs::path anc_lock = cur / ".locus" / "locus.lock";
        std::error_code ec;
        if (fs::exists(anc_lock, ec) && !ec) {
            if (!ancestor_is_stale(anc_lock)) {
                std::string who = read_lock_diagnostic(anc_lock);
                std::string msg = "Workspace conflict: an ancestor folder '" +
                                  cur.string() +
                                  "' is already open as a Locus workspace";
                if (!who.empty()) msg += " (" + who + ")";
                msg += ". Close that instance first, or pick a folder outside it.";
                throw std::runtime_error(msg);
            }
        }
        cur = cur.parent_path();
    }
}

} // namespace

// -- Platform-specific lock acquisition -------------------------------------

#ifdef _WIN32

WorkspaceLock::WorkspaceLock(const fs::path& workspace_root)
{
    fs::path locus_dir = workspace_root / ".locus";
    std::error_code ec;
    fs::create_directories(locus_dir, ec);
    if (ec)
        throw std::runtime_error("Cannot create .locus/: " + ec.message());
    lock_path_ = locus_dir / "locus.lock";

    check_ancestors(workspace_root);

    std::wstring w = lock_path_.wstring();
    HANDLE h = CreateFileW(w.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot open workspace lock file: " +
                                 lock_path_.string());
    }

    OVERLAPPED ov{};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, MAXDWORD, MAXDWORD, &ov)) {
        std::string who = read_lock_diagnostic(lock_path_);
        CloseHandle(h);
        std::string msg = "Workspace '" + workspace_root.string() +
                          "' is already open in another Locus instance";
        if (!who.empty()) msg += " (" + who + ")";
        msg += ". Close the other instance and try again.";
        throw std::runtime_error(msg);
    }
    handle_ = h;

    // Overwrite the lock file with our diagnostic JSON. Fixed-size layout not
    // required — the file is only ever read for human-readable hints.
    json info;
    info["pid"]     = static_cast<long long>(GetCurrentProcessId());
    info["started"] = now_iso8601();
    info["path"]    = workspace_root.string();
    std::string payload = info.dump(2);

    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    DWORD written = 0;
    WriteFile(h, payload.data(), static_cast<DWORD>(payload.size()),
              &written, nullptr);
    FlushFileBuffers(h);

    spdlog::info("Workspace lock acquired: {}", lock_path_.string());
}

WorkspaceLock::~WorkspaceLock()
{
    if (handle_) {
        OVERLAPPED ov{};
        UnlockFileEx(static_cast<HANDLE>(handle_), 0,
                     MAXDWORD, MAXDWORD, &ov);
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
        spdlog::trace("Workspace lock released: {}", lock_path_.string());
    }
}

#else

WorkspaceLock::WorkspaceLock(const fs::path& workspace_root)
{
    fs::path locus_dir = workspace_root / ".locus";
    std::error_code ec;
    fs::create_directories(locus_dir, ec);
    if (ec)
        throw std::runtime_error("Cannot create .locus/: " + ec.message());
    lock_path_ = locus_dir / "locus.lock";

    check_ancestors(workspace_root);

    fd_ = ::open(lock_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0)
        throw std::runtime_error("Cannot open workspace lock file: " +
                                 lock_path_.string());

    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        std::string who = read_lock_diagnostic(lock_path_);
        ::close(fd_);
        fd_ = -1;
        std::string msg = "Workspace '" + workspace_root.string() +
                          "' is already open in another Locus instance";
        if (!who.empty()) msg += " (" + who + ")";
        msg += ". Close the other instance and try again.";
        throw std::runtime_error(msg);
    }

    json info;
    info["pid"]     = static_cast<long long>(::getpid());
    info["started"] = now_iso8601();
    info["path"]    = workspace_root.string();
    std::string payload = info.dump(2);

    ::ftruncate(fd_, 0);
    ::lseek(fd_, 0, SEEK_SET);
    ::write(fd_, payload.data(), payload.size());

    spdlog::info("Workspace lock acquired: {}", lock_path_.string());
}

WorkspaceLock::~WorkspaceLock()
{
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
        spdlog::trace("Workspace lock released: {}", lock_path_.string());
    }
}

#endif

} // namespace locus

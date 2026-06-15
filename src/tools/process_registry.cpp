#include "process_registry.h"
#include "process_sink.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#define LOCUS_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define LOCUS_ENVIRON environ
#endif
#endif

namespace locus {

namespace {

int64_t now_unix()
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

const char* to_string(BackgroundProcess::Status s)
{
    switch (s) {
        case BackgroundProcess::Status::running: return "running";
        case BackgroundProcess::Status::exited:  return "exited";
        case BackgroundProcess::Status::killed:  return "killed";
    }
    return "unknown";
}

// -- BackgroundProcess ------------------------------------------------------

BackgroundProcess::BackgroundProcess(int id, std::string command, std::size_t buffer_cap,
                                     ProcessSinkBroker* sink_broker)
    : id_(id),
      command_(std::move(command)),
      started_at_unix_(now_unix()),
      buffer_cap_(buffer_cap),
      sink_broker_(sink_broker)
{
    // Reserve up front so the ring buffer never reallocates after the cap is
    // hit -- keeps the reader thread out of the allocator on hot output.
    buffer_.reserve(buffer_cap_ > 0 ? buffer_cap_ : std::size_t{4096});
}

BackgroundProcess::~BackgroundProcess()
{
    terminate();
    join();
}

void BackgroundProcess::join()
{
#ifdef _WIN32
    // Close the stdin pipe first so the child sees EOF if it's still reading;
    // do it under the mutex because `write_stdin` may be observing the handle
    // from another thread.
    {
        std::lock_guard l(mu_);
        if (stdin_write_) {
            CloseHandle(static_cast<HANDLE>(stdin_write_));
            stdin_write_ = nullptr;
        }
    }
    if (reader_.joinable()) reader_.join();

    if (read_pipe_) {
        CloseHandle(static_cast<HANDLE>(read_pipe_));
        read_pipe_ = nullptr;
    }
    if (process_) {
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (job_) {
        CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
#else
    // Close the stdin pipe so a child still reading sees EOF; mirror the
    // Windows ordering (under the mutex, before joining the reader).
    {
        std::lock_guard l(mu_);
        if (stdin_fd_ >= 0) {
            ::close(stdin_fd_);
            stdin_fd_ = -1;
        }
    }
    if (reader_.joinable()) reader_.join();
    if (read_fd_ >= 0) {
        ::close(read_fd_);
        read_fd_ = -1;
    }
    // pid_ was already reaped by the reader thread's waitpid; nothing else to
    // release (no OS handle object on POSIX).
#endif
}

BackgroundProcess::Status BackgroundProcess::status() const
{
    std::lock_guard l(mu_);
    return status_;
}

int BackgroundProcess::exit_code() const
{
    std::lock_guard l(mu_);
    return exit_code_;
}

std::size_t BackgroundProcess::total_bytes() const
{
    std::lock_guard l(mu_);
    return total_bytes_received_;
}

void BackgroundProcess::append_locked(const char* data, std::size_t n)
{
    // S5.B -- fan out to the terminal panel *before* the ring buffer cap is
    // applied so the panel sees every byte even when the LLM-facing buffer
    // drops old output. The sink is non-owning + thread-safe.
    if (sink_broker_) sink_broker_->emit_bg_chunk(id_, data, n);

    total_bytes_received_ += n;
    if (buffer_cap_ == 0) {
        buffer_.append(data, n);
        return;
    }
    if (n >= buffer_cap_) {
        // The new chunk alone exceeds the cap -- keep only its tail.
        std::size_t keep = buffer_cap_;
        std::size_t drop_old = buffer_.size();
        bytes_dropped_ += drop_old + (n - keep);
        buffer_.assign(data + (n - keep), keep);
        return;
    }
    if (buffer_.size() + n > buffer_cap_) {
        std::size_t overflow = buffer_.size() + n - buffer_cap_;
        bytes_dropped_ += overflow;
        buffer_.erase(0, overflow);
    }
    buffer_.append(data, n);
}

BackgroundProcess::ReadResult BackgroundProcess::read_output(std::size_t since_offset)
{
    std::lock_guard l(mu_);

    ReadResult r;
    r.status      = status_;
    r.exit_code   = exit_code_;
    r.next_offset = total_bytes_received_;

    // Window in the monotonic byte stream that is still buffered:
    // [bytes_dropped_ .. total_bytes_received_).
    std::size_t window_start = bytes_dropped_;
    std::size_t window_end   = total_bytes_received_;

    if (since_offset >= window_end) {
        // Caller is up to date.
        r.dropped_before_window = 0;
        return r;
    }

    std::size_t read_from;
    if (since_offset < window_start) {
        r.dropped_before_window = window_start - since_offset;
        read_from = window_start;
    } else {
        r.dropped_before_window = 0;
        read_from = since_offset;
    }

    std::size_t buffer_offset = read_from - window_start;
    r.data.assign(buffer_, buffer_offset, window_end - read_from);
    return r;
}

void BackgroundProcess::terminate()
{
#ifdef _WIN32
    // Snapshot the job under lock. TerminateJobObject is async -- the reader
    // thread will observe the pipe close and exit on its own.
    HANDLE job_handle = nullptr;
    {
        std::lock_guard l(mu_);
        if (status_ != Status::running) return;
        status_ = Status::killed;
        job_handle = static_cast<HANDLE>(job_);
    }
    if (job_handle) {
        TerminateJobObject(job_handle, 1);
    }
#else
    int pgid = -1;
    {
        std::lock_guard l(mu_);
        if (status_ != Status::running) return;
        status_ = Status::killed;
        pgid = pgid_;
    }
    // SIGTERM the whole process group (the `sh -c` leader + every child it
    // forked share pgid_). The reader thread observes the resulting pipe EOF
    // and reaps via waitpid. killpg on an already-dead group returns ESRCH,
    // which is harmless here.
    if (pgid > 0) ::killpg(pgid, SIGTERM);
#endif
}

bool BackgroundProcess::write_stdin(std::string_view data)
{
#ifdef _WIN32
    if (data.empty()) return true;
    std::lock_guard l(mu_);
    if (status_ != Status::running) return false;
    if (!stdin_write_) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(static_cast<HANDLE>(stdin_write_), data.data(),
                        static_cast<DWORD>(data.size()), &written, nullptr);
    if (!ok || written != data.size()) {
        spdlog::warn("BackgroundProcess::write_stdin: WriteFile failed id={} err={}",
                     id_, ok ? 0 : GetLastError());
        return false;
    }
    return true;
#else
    if (data.empty()) return true;
    std::lock_guard l(mu_);
    if (status_ != Status::running) return false;
    if (stdin_fd_ < 0) return false;
    std::size_t total = 0;
    while (total < data.size()) {
        ssize_t w = ::write(stdin_fd_, data.data() + total, data.size() - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            spdlog::warn("BackgroundProcess::write_stdin: write failed id={} err={}",
                         id_, std::strerror(errno));
            return false;
        }
        total += static_cast<std::size_t>(w);
    }
    return true;
#endif
}

#ifdef _WIN32

void BackgroundProcess::spawn_win32(const fs::path& cwd)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        throw std::runtime_error("CreatePipe failed");

    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    // S5.Z task 4 -- give the child a real stdin pipe so the terminal panel
    // can forward user input. The parent-side write handle (`stdin_write`)
    // is marked non-inheritable so a re-spawn doesn't leak it.
    HANDLE stdin_read  = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        throw std::runtime_error("CreatePipe (stdin) failed");
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (!job) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        throw std::runtime_error("CreateJobObject failed");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
    jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jli, sizeof(jli));

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError  = write_pipe;
    si.hStdInput  = stdin_read;

    PROCESS_INFORMATION pi{};
    std::string cmd_line = "cmd /c " + command_;

    BOOL created = CreateProcessA(
        nullptr, cmd_line.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, cwd.string().c_str(),
        &si, &pi);

    CloseHandle(write_pipe);
    CloseHandle(stdin_read);

    if (!created) {
        DWORD err = GetLastError();
        CloseHandle(read_pipe);
        CloseHandle(stdin_write);
        CloseHandle(job);
        throw std::runtime_error("CreateProcess failed (" + std::to_string(err) + ")");
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(read_pipe);
        CloseHandle(stdin_write);
        CloseHandle(job);
        throw std::runtime_error("AssignProcessToJobObject failed (" +
                                 std::to_string(err) + ")");
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    process_     = pi.hProcess;
    job_         = job;
    read_pipe_   = read_pipe;
    stdin_write_ = stdin_write;

    reader_running_ = true;
    reader_ = std::thread(&BackgroundProcess::reader_loop, this);
}

void BackgroundProcess::reader_loop()
{
    char buf[4096];
    DWORD bytes = 0;
    HANDLE pipe = static_cast<HANDLE>(read_pipe_);
    while (ReadFile(pipe, buf, sizeof(buf), &bytes, nullptr) && bytes > 0) {
        std::lock_guard l(mu_);
        append_locked(buf, bytes);
    }

    // Pipe closed -- child + descendants have all exited (or the job was
    // terminated). Capture the exit code if we weren't explicitly killed.
    HANDLE proc = static_cast<HANDLE>(process_);
    int    final_code = 0;
    bool   was_killed = false;
    if (proc) {
        WaitForSingleObject(proc, 2000);
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        std::lock_guard l(mu_);
        exit_code_ = static_cast<int>(code);
        if (status_ == Status::running)
            status_ = Status::exited;
        final_code = exit_code_;
        was_killed = (status_ == Status::killed);
    }
    reader_running_ = false;

    // Fan out the lifecycle event AFTER releasing the per-process mutex so
    // the sink can take its own locks without inversion risk.
    if (sink_broker_) sink_broker_->emit_bg_exited(id_, final_code, was_killed);
}

#else // !_WIN32

void BackgroundProcess::spawn_posix(const fs::path& cwd)
{
    // stdout+stderr pipe (child writes [1], parent reads [0]) and a stdin pipe
    // (child reads [0], parent writes [1]).
    int out_pipe[2] = {-1, -1};
    int in_pipe[2]  = {-1, -1};
    if (::pipe(out_pipe) != 0)
        throw std::runtime_error("pipe (stdout) failed");
    if (::pipe(in_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        throw std::runtime_error("pipe (stdin) failed");
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Run in the workspace root. The _np spelling works from macOS 10.15;
    // the non-_np form only arrived in macOS 26, so keep _np for back-compat
    // and silence its deprecation note.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    posix_spawn_file_actions_addchdir_np(&fa, cwd.string().c_str());
#pragma clang diagnostic pop
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0],  STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDERR_FILENO);
    // Close every inherited pipe fd in the child after the dup2s.
    posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[1]);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Make the child its own process-group leader so killpg(pid, ...) kills the
    // whole `sh -c 'foo && bar'` subtree -- the macOS analogue of the Win32
    // KILL_ON_JOB_CLOSE job object.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    std::string cmd = command_;
    char* const argv[] = {
        const_cast<char*>("/bin/sh"),
        const_cast<char*>("-c"),
        const_cast<char*>(cmd.c_str()),
        nullptr,
    };

    pid_t pid = -1;
    int rc = posix_spawn(&pid, "/bin/sh", &fa, &attr, argv, LOCUS_ENVIRON);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    // Parent closes the child ends regardless of outcome.
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);

    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        throw std::runtime_error(std::string("posix_spawn failed: ") + std::strerror(rc));
    }

    // Parent-side stdin write end is non-inheritable so a later spawn can't
    // leak it (mirrors the Win32 SetHandleInformation(stdin_write, ..., 0)).
    ::fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);

    pid_      = pid;
    pgid_     = pid;  // we made the child its own group leader
    read_fd_  = out_pipe[0];
    stdin_fd_ = in_pipe[1];

    reader_running_ = true;
    reader_ = std::thread(&BackgroundProcess::reader_loop, this);
}

void BackgroundProcess::reader_loop()
{
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(read_fd_, buf, sizeof(buf));
        if (n > 0) {
            std::lock_guard l(mu_);
            append_locked(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;                       // EOF: all write ends closed
        } else {
            if (errno == EINTR) continue;
            break;                       // read error
        }
    }

    // Pipe drained -- the child and its group have exited (or were killed).
    // Reap to get the exit status and avoid a zombie.
    int    final_code = 0;
    bool   was_killed = false;
    if (pid_ > 0) {
        int wstatus = 0;
        while (::waitpid(pid_, &wstatus, 0) < 0 && errno == EINTR) {}
        int code = 0;
        if (WIFEXITED(wstatus))        code = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus)) code = 128 + WTERMSIG(wstatus);

        std::lock_guard l(mu_);
        exit_code_ = code;
        if (status_ == Status::running)
            status_ = Status::exited;
        final_code = exit_code_;
        was_killed = (status_ == Status::killed);
    }
    reader_running_ = false;

    if (sink_broker_) sink_broker_->emit_bg_exited(id_, final_code, was_killed);
}

#endif

// -- ProcessRegistry --------------------------------------------------------

ProcessRegistry::ProcessRegistry(fs::path workspace_root, std::size_t buffer_cap_bytes,
                                 ProcessSinkBroker* external_broker)
    : root_(std::move(workspace_root)),
      buffer_cap_(buffer_cap_bytes),
      owned_broker_(external_broker ? nullptr : std::make_unique<ProcessSinkBroker>()),
      broker_ptr_(external_broker ? external_broker : owned_broker_.get())
{}

ProcessRegistry::~ProcessRegistry()
{
    // Move the map out under the lock so the unique_ptr destructors (which
    // call terminate() + join()) run without holding the registry mutex.
    decltype(procs_) procs;
    {
        std::lock_guard l(mu_);
        procs.swap(procs_);
    }
    if (!procs.empty())
        spdlog::info("ProcessRegistry: terminating {} background process(es) on close",
                     procs.size());
    procs.clear();
}

int ProcessRegistry::spawn(const std::string& command)
{
    int id;
    std::unique_ptr<BackgroundProcess> proc;
    {
        std::lock_guard l(mu_);
        id = next_id_++;
        proc.reset(new BackgroundProcess(id, command, buffer_cap_, broker_ptr_));
    }

    // spawn_* throws on failure; do not insert a half-constructed entry.
#ifdef _WIN32
    proc->spawn_win32(root_);
#else
    proc->spawn_posix(root_);
#endif

    {
        std::lock_guard l(mu_);
        procs_.emplace(id, std::move(proc));
        last_delivered_[id] = 0;
    }
    spdlog::info("Background process spawned: id={} cmd='{}'", id, command);
    // S5.B -- announce the new process to the terminal panel after the
    // entry is in the registry, so a tab-creation handler that immediately
    // queries `list()` sees the row.
    if (broker_ptr_) broker_ptr_->emit_bg_started(id, command);
    return id;
}

std::optional<BackgroundProcess::ReadResult>
ProcessRegistry::read_output(int id, std::optional<std::size_t> since_offset)
{
    BackgroundProcess* proc = nullptr;
    std::size_t offset = 0;
    {
        std::lock_guard l(mu_);
        auto it = procs_.find(id);
        if (it == procs_.end()) return std::nullopt;
        proc = it->second.get();
        if (since_offset.has_value()) {
            offset = *since_offset;
        } else {
            offset = last_delivered_[id];
        }
    }

    auto r = proc->read_output(offset);

    if (!since_offset.has_value()) {
        std::lock_guard l(mu_);
        last_delivered_[id] = r.next_offset;
    }
    return r;
}

bool ProcessRegistry::stop(int id)
{
    std::lock_guard l(mu_);
    auto it = procs_.find(id);
    if (it == procs_.end()) return false;
    it->second->terminate();
    return true;
}

bool ProcessRegistry::write_stdin(int id, std::string_view data)
{
    BackgroundProcess* proc = nullptr;
    {
        std::lock_guard l(mu_);
        auto it = procs_.find(id);
        if (it == procs_.end()) return false;
        proc = it->second.get();
    }
    return proc->write_stdin(data);
}

bool ProcessRegistry::remove(int id)
{
    std::unique_ptr<BackgroundProcess> doomed;
    {
        std::lock_guard l(mu_);
        auto it = procs_.find(id);
        if (it == procs_.end()) return false;
        doomed = std::move(it->second);
        procs_.erase(it);
        last_delivered_.erase(id);
    }
    // Run terminate+join outside the registry mutex.
    doomed.reset();
    return true;
}

std::vector<ProcessRegistry::ListEntry> ProcessRegistry::list()
{
    std::lock_guard l(mu_);
    std::vector<ListEntry> out;
    out.reserve(procs_.size());
    for (auto& [id, proc] : procs_) {
        ListEntry e;
        e.id              = id;
        e.command         = proc->command();
        e.started_at_unix = proc->started_at_unix();
        e.status          = proc->status();
        e.exit_code       = proc->exit_code();
        std::size_t total = proc->total_bytes();
        std::size_t last  = last_delivered_[id];
        e.bytes_pending   = total > last ? (total - last) : 0;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(),
              [](const ListEntry& a, const ListEntry& b) { return a.id < b.id; });
    return out;
}

} // namespace locus

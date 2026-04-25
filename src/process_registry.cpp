#include "process_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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

BackgroundProcess::BackgroundProcess(int id, std::string command, std::size_t buffer_cap)
    : id_(id),
      command_(std::move(command)),
      started_at_unix_(now_unix()),
      buffer_cap_(buffer_cap)
{
    // Reserve up front so the ring buffer never reallocates after the cap is
    // hit — keeps the reader thread out of the allocator on hot output.
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
    total_bytes_received_ += n;
    if (buffer_cap_ == 0) {
        buffer_.append(data, n);
        return;
    }
    if (n >= buffer_cap_) {
        // The new chunk alone exceeds the cap — keep only its tail.
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
    // Snapshot the job under lock. TerminateJobObject is async — the reader
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

    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (!job) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
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
    // Detach stdin: a backgrounded `cmd /c` should never block reading from
    // our terminal. NUL is the safe sink.
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul;

    PROCESS_INFORMATION pi{};
    std::string cmd_line = "cmd /c " + command_;

    BOOL created = CreateProcessA(
        nullptr, cmd_line.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, cwd.string().c_str(),
        &si, &pi);

    CloseHandle(write_pipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!created) {
        DWORD err = GetLastError();
        CloseHandle(read_pipe);
        CloseHandle(job);
        throw std::runtime_error("CreateProcess failed (" + std::to_string(err) + ")");
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(read_pipe);
        CloseHandle(job);
        throw std::runtime_error("AssignProcessToJobObject failed (" +
                                 std::to_string(err) + ")");
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    process_   = pi.hProcess;
    job_       = job;
    read_pipe_ = read_pipe;

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

    // Pipe closed — child + descendants have all exited (or the job was
    // terminated). Capture the exit code if we weren't explicitly killed.
    HANDLE proc = static_cast<HANDLE>(process_);
    if (proc) {
        WaitForSingleObject(proc, 2000);
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        std::lock_guard l(mu_);
        exit_code_ = static_cast<int>(code);
        if (status_ == Status::running)
            status_ = Status::exited;
    }
    reader_running_ = false;
}

#else // !_WIN32

void BackgroundProcess::reader_loop() {}

#endif

// -- ProcessRegistry --------------------------------------------------------

ProcessRegistry::ProcessRegistry(fs::path workspace_root, std::size_t buffer_cap_bytes)
    : root_(std::move(workspace_root)),
      buffer_cap_(buffer_cap_bytes)
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
#ifdef _WIN32
    int id;
    std::unique_ptr<BackgroundProcess> proc;
    {
        std::lock_guard l(mu_);
        id = next_id_++;
        proc.reset(new BackgroundProcess(id, command, buffer_cap_));
    }

    // spawn_win32 throws on failure; do not insert a half-constructed entry.
    proc->spawn_win32(root_);

    {
        std::lock_guard l(mu_);
        procs_.emplace(id, std::move(proc));
        last_delivered_[id] = 0;
    }
    spdlog::info("Background process spawned: id={} cmd='{}'", id, command);
    return id;
#else
    (void)command;
    throw std::runtime_error("Background commands not implemented on this platform");
#endif
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

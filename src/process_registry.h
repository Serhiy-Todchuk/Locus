#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
// Forward-declare opaque HANDLE (void*) without dragging windows.h into this
// header — keeps compile times sane for callers that just want the registry.
using LocusOsHandle = void*;
#endif

namespace locus {

namespace fs = std::filesystem;

// One long-running shell command spawned by the agent. Owns the Win32 process
// handle, an output ring buffer, and a background thread that drains the
// process's stdout+stderr pipe. Lifetime is tied to the owning ProcessRegistry.
//
// Threading: every accessor takes the per-process mutex except for the
// immutable identity fields (`id`, `command`, `started_at_unix`).
class BackgroundProcess {
public:
    enum class Status { running, exited, killed };

    int                id() const                 { return id_; }
    const std::string& command() const            { return command_; }
    int64_t            started_at_unix() const    { return started_at_unix_; }

    Status status() const;
    int    exit_code() const;
    // Total bytes the reader has appended to the buffer (monotonic, includes
    // bytes that have since been dropped by the ring buffer).
    std::size_t total_bytes() const;

    struct ReadResult {
        std::string data;
        std::size_t next_offset;          // pass back as since_offset for the next read
        std::size_t dropped_before_window; // bytes between since_offset and the start of `data` that were lost to the ring buffer cap
        Status      status;
        int         exit_code;
    };
    // Read the slice of buffered output starting at `since_offset`.
    ReadResult read_output(std::size_t since_offset);

    // Terminate the process tree (TerminateJobObject on Windows). Idempotent.
    void terminate();

    // Wait for the reader thread to drain and join. Called by the registry on
    // teardown after `terminate()` to ensure no thread outlives this object.
    void join();

    BackgroundProcess(const BackgroundProcess&)            = delete;
    BackgroundProcess& operator=(const BackgroundProcess&) = delete;

    ~BackgroundProcess();

private:
    friend class ProcessRegistry;

    BackgroundProcess(int id, std::string command, std::size_t buffer_cap);

#ifdef _WIN32
    // Spawn under the given working directory. Throws std::runtime_error on
    // failure. Caller already holds the registry mutex.
    void spawn_win32(const fs::path& cwd);
#endif

    void reader_loop();
    void append_locked(const char* data, std::size_t n); // mu_ held

    const int          id_;
    const std::string  command_;
    const int64_t      started_at_unix_;
    const std::size_t  buffer_cap_;

    mutable std::mutex mu_;
    Status             status_     = Status::running;
    int                exit_code_  = 0;
    std::string        buffer_;             // tail of output, capped at buffer_cap_
    std::size_t        total_bytes_received_ = 0;
    std::size_t        bytes_dropped_       = 0;

#ifdef _WIN32
    LocusOsHandle      process_   = nullptr;
    LocusOsHandle      job_       = nullptr;
    LocusOsHandle      read_pipe_ = nullptr;
    std::thread        reader_;
#endif
    std::atomic<bool>  reader_running_{false};
};

// Per-workspace process registry. Spawns, tracks, and kills background
// processes spawned by the agent. Owned by Workspace; destructor terminates
// every still-running child so the agent never leaks processes when the
// workspace closes.
class ProcessRegistry {
public:
    ProcessRegistry(fs::path workspace_root, std::size_t buffer_cap_bytes);
    ~ProcessRegistry();

    ProcessRegistry(const ProcessRegistry&)            = delete;
    ProcessRegistry& operator=(const ProcessRegistry&) = delete;

    // Spawn a new background process running `command` in the workspace root.
    // Returns the assigned id. Throws std::runtime_error on spawn failure.
    int spawn(const std::string& command);

    // Read buffered output for `id`. If `since_offset` is std::nullopt, the
    // registry reads "everything new since the last read of this pid by this
    // tool" — it keeps the per-pid bookkeeping so the LLM doesn't have to.
    // Returns std::nullopt if `id` is unknown.
    std::optional<BackgroundProcess::ReadResult>
    read_output(int id, std::optional<std::size_t> since_offset);

    // Terminate a process tree. Returns false if `id` is unknown.
    bool stop(int id);

    // Remove an entry from the registry. Running processes are terminated
    // first. Returns false if `id` is unknown.
    bool remove(int id);

    struct ListEntry {
        int                       id;
        std::string               command;
        int64_t                   started_at_unix;
        BackgroundProcess::Status status;
        int                       exit_code;
        std::size_t               bytes_pending;   // total - last delivered offset
    };
    std::vector<ListEntry> list();

    std::size_t buffer_cap() const { return buffer_cap_; }

private:
    fs::path     root_;
    std::size_t  buffer_cap_;

    std::mutex   mu_;
    int          next_id_ = 1;
    std::unordered_map<int, std::unique_ptr<BackgroundProcess>> procs_;
    std::unordered_map<int, std::size_t>                        last_delivered_;
};

const char* to_string(BackgroundProcess::Status s);

} // namespace locus

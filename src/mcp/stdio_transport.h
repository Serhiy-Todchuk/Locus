#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace locus {

#ifdef _WIN32
using LocusOsHandle = void*;
#endif

// Spawns a child process with redirected stdin + stdout and exposes a
// line-oriented send/receive surface. The child's stderr is inherited from
// the parent so MCP servers' debug logs land in the user's terminal /
// activity log without us having to explicitly drain a third pipe.
//
// Threading: a dedicated reader thread drains stdout, splits on '\n', and
// fires `on_message` for each complete JSON line. `send_line()` is called
// from arbitrary threads -- writes are mutex-serialised. Pipe close (i.e.
// the child exited) fires `on_closed` once and stops the reader thread.
class StdioTransport {
public:
    StdioTransport();
    ~StdioTransport();

    StdioTransport(const StdioTransport&)            = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    // Spawn the child. `env_overrides` is merged on top of the parent's
    // environment (overrides win); pass an empty map to inherit unchanged.
    // Throws std::runtime_error on failure.
    void spawn(const std::string&                       command,
               const std::vector<std::string>&          args,
               const std::map<std::string, std::string>& env_overrides,
               const std::filesystem::path&             cwd);

    // Wire callbacks. Both fire on the reader thread.
    void on_message(std::function<void(std::string)> cb) { msg_cb_ = std::move(cb); }
    void on_closed (std::function<void()>            cb) { closed_cb_ = std::move(cb); }

    // Write a single newline-terminated message. Returns false if the child
    // is no longer running or the pipe has been closed.
    bool send_line(const std::string& json_line);

    // Status accessors (never block).
    bool is_running() const { return running_.load(); }

    // Forcibly terminate the child + reader thread. Idempotent.
    void terminate();

private:
    void reader_loop();

    std::atomic<bool>                running_{false};
    std::function<void(std::string)> msg_cb_;
    std::function<void()>            closed_cb_;

#ifdef _WIN32
    LocusOsHandle process_  = nullptr;
    LocusOsHandle job_      = nullptr;   // KILL_ON_JOB_CLOSE so the child dies with us
    LocusOsHandle stdin_w_  = nullptr;   // we write -> child stdin
    LocusOsHandle stdout_r_ = nullptr;   // child stdout -> we read
#endif
    std::mutex   write_mu_;              // serialises send_line()
    std::thread  reader_;
};

} // namespace locus

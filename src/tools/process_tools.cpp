#include "tools/process_tools.h"
#include "tools/process_output_filter.h"
#include "tools/process_sink.h"
#include "tools/shared.h"

#include "core/workspace.h"
#include "core/workspace_services.h"
#include "process_registry.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace locus {

using tools::error_result;

std::string RunCommandTool::preview(const ToolCall& call) const
{
    std::string cmd = call.args.value("command", "");
    return "Execute: " + cmd;
}

#ifdef _WIN32

ToolResult RunCommandTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                    const std::atomic<bool>* cancel_flag)
{
    std::string command = call.args.value("command", "");
    // Default 30 minutes -- matches Pi's bash tool. Long-enough builds, test
    // suites, and one-shot scripts complete without the agent having to
    // override the cap; truly long-running things (servers, watchers) still
    // belong on `run_command_bg`.
    int timeout_ms = call.args.value("timeout_ms", 1800000);

    if (command.empty())
        return error_result("Error: 'command' parameter is required");

    spdlog::trace("run_command: '{}'", command);

    // S5.B -- if the terminal panel is attached, stream stdout/stderr chunks
    // into it concurrently with the wait. The sink is null on CLI / headless
    // runs; in that case the pre-S5.B "drain after wait" path is retained.
    ProcessSinkBroker* sink = ws.process_sink();
    if (sink) sink->emit_sync_started(command);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return error_result("Error: failed to create pipe");

    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    // Job object: kills the entire child tree on timeout. Without this,
    // `cmd /c ping ...` would launch ping as a grandchild, and TerminateProcess
    // on cmd would orphan ping -- its inherited write handle would keep our
    // ReadFile loop blocked until ping naturally exited (~120s for `ping -n 120`).
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (!job) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return error_result("Error: failed to create job object");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
    jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jli, sizeof(jli));

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError  = write_pipe;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    std::string cmd_line = "cmd /c " + command;

    // CREATE_SUSPENDED so we can attach the job before the child can fork
    // grandchildren that would escape the job (the kernel propagates job
    // membership to descendants only at process creation time).
    BOOL created = CreateProcessA(
        nullptr,
        cmd_line.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr,
        ws.root().string().c_str(),
        &si, &pi
    );

    CloseHandle(write_pipe);

    if (!created) {
        CloseHandle(read_pipe);
        CloseHandle(job);
        return error_result("Error: failed to create process (error " +
                            std::to_string(GetLastError()) + ")");
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        // Failed to put the child in the job -- kill it before it spawns
        // grandchildren we can't track.
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(read_pipe);
        CloseHandle(job);
        return error_result("Error: AssignProcessToJobObject failed (" +
                            std::to_string(GetLastError()) + ")");
    }

    ResumeThread(pi.hThread);

    // Instrumentation (added after Tetris agentic testing exposed a
    // reproducible reader-thread hang on second/later run_command calls --
    // see tests/ui_automation/output/agentic_Tetris/findings.md findings 7+8).
    // Every signal here is at info so the post-mortem `read_locus_log` op
    // surfaces them without needing -verbose; under the agentic mute-noise
    // mode they stay visible (only db + fs noise loggers are demoted).
    const DWORD child_pid = pi.dwProcessId;
    const auto t_spawn = std::chrono::steady_clock::now();
    spdlog::info("run_command[pid={}]: spawned ({} chars)", child_pid, cmd_line.size());

    // Drain the pipe on a background thread so the terminal panel (if any)
    // sees chunks live rather than as one wall of text after exit. We still
    // capture the full output for the agent's tool result. WaitForSingleObject
    // returns when the process exits; the job-kill on timeout closes the
    // write end so the reader thread observes EOF and joins.
    std::string  output;
    std::mutex   output_mu;

    // Reader-state snapshot shared with a heartbeat thread. The heartbeat
    // logs every 30s while the reader is alive so a stuck ReadFile is
    // immediately visible in the log (the bug from findings 7+8 left the
    // log silent for 40+ min with no indication anything was wrong).
    std::atomic<uint64_t> reader_bytes_total{0};
    std::atomic<uint32_t> reader_read_calls{0};
    std::atomic<DWORD>    reader_last_err{ERROR_SUCCESS};
    std::atomic<bool>     reader_done{false};
    std::atomic<bool>     child_exited{false};

    std::thread reader([&]{
        spdlog::trace("run_command[pid={}]: reader thread start", child_pid);
        char buf[4096];
        DWORD bytes_read = 0;
        while (ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
            reader_bytes_total.fetch_add(bytes_read, std::memory_order_relaxed);
            reader_read_calls.fetch_add(1, std::memory_order_relaxed);
            if (sink) sink->emit_sync_chunk(buf, bytes_read);
            std::lock_guard l(output_mu);
            output.append(buf, bytes_read);
        }
        reader_last_err.store(GetLastError(), std::memory_order_relaxed);
        reader_done.store(true, std::memory_order_release);
        spdlog::trace(
            "run_command[pid={}]: reader thread done ({} bytes, {} reads, last_err={})",
            child_pid, reader_bytes_total.load(), reader_read_calls.load(),
            reader_last_err.load());
    });

    // Heartbeat -- fires only if the reader is still running after the child
    // has exited, which is the symptom of the inherited-handle pipe-leak bug.
    // Quiet while everything is healthy.
    //
    // S5.Z follow-up -- when WorkspaceConfig::Agent::dump_on_run_command_hang is on
    // AND the heartbeat catches a stuck reader past a child exit, the
    // heartbeat shells out to `procdump.exe -ma <self_pid> <dump_path>` so we
    // capture an in-vivo snapshot of the agent thread's ReadFile stack. One
    // dump per RunCommandTool::execute call to keep disk usage bounded.
    bool dump_on_hang = false;
    fs::path dumps_dir;
    if (auto* w = ws.workspace()) {
        if (w->config().agent.dump_on_run_command_hang) {
            dump_on_hang = true;
            dumps_dir = w->root() / ".locus" / "dumps";
        }
    }
    std::atomic<bool> dump_taken{false};

    std::mutex hb_mu;
    std::condition_variable hb_cv;
    bool hb_stop = false;
    std::thread heartbeat([&]{
        constexpr auto interval = std::chrono::seconds(30);
        std::unique_lock<std::mutex> lk(hb_mu);
        while (!hb_stop) {
            if (hb_cv.wait_for(lk, interval, [&]{ return hb_stop; }))
                break;
            if (reader_done.load(std::memory_order_acquire))
                break;
            auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_spawn).count();
            const bool child_done = child_exited.load();
            spdlog::warn(
                "run_command[pid={}]: reader still draining after {} ms "
                "({} bytes, {} reads, child_exited={})",
                child_pid, (long long)wall_ms,
                reader_bytes_total.load(), reader_read_calls.load(),
                child_done ? "true" : "false");

            if (dump_on_hang && child_done && !dump_taken.load()) {
                // Compose the dump filename. Best-effort: if procdump isn't
                // on PATH the CreateProcess call below silently fails -- the
                // warn line above still pinpoints the bug. We avoid any
                // synchronous wait because the agent thread is stuck and we
                // don't want to add a third blocked thread.
                std::error_code dec;
                fs::create_directories(dumps_dir, dec);
                auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                fs::path dump_file = dumps_dir /
                    ("run_command_hang_" + std::to_string(now_s) + "_pid" +
                     std::to_string(GetCurrentProcessId()) + ".dmp");
                std::string cmd_line = "procdump.exe -accepteula -ma " +
                    std::to_string(GetCurrentProcessId()) + " \"" +
                    dump_file.string() + "\"";
                STARTUPINFOA dsi{}; dsi.cb = sizeof(dsi);
                PROCESS_INFORMATION dpi{};
                std::vector<char> cl(cmd_line.begin(), cmd_line.end()); cl.push_back('\0');
                BOOL ok = CreateProcessA(nullptr, cl.data(), nullptr, nullptr,
                                         FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                         &dsi, &dpi);
                if (ok) {
                    spdlog::warn(
                        "run_command[pid={}]: procdump launched -> {}",
                        child_pid, dump_file.string());
                    CloseHandle(dpi.hThread);
                    CloseHandle(dpi.hProcess);
                    dump_taken.store(true);
                } else {
                    spdlog::warn(
                        "run_command[pid={}]: procdump launch failed "
                        "(err={}; install Sysinternals procdump on PATH to enable)",
                        child_pid, GetLastError());
                    dump_taken.store(true);  // don't retry this call
                }
            }
        }
    });

    // S5.Z task 7 -- replace the single WaitForSingleObject with a polled
    // wait so the dispatcher's cancel flag is observed in <= poll_ms.
    // Polling interval is short (50 ms) -- the WaitForSingleObject still
    // blocks the agent thread, but the wake-up is now bounded by both the
    // process exiting AND the user pressing Stop.
    constexpr DWORD k_poll_ms = 50;
    DWORD remaining = timeout_ms <= 0 ? INFINITE
                                       : static_cast<DWORD>(timeout_ms);
    DWORD wait_result = WAIT_FAILED;
    bool cancelled = false;
    for (;;) {
        DWORD slice = (remaining == INFINITE)
                          ? k_poll_ms
                          : std::min<DWORD>(remaining, k_poll_ms);
        wait_result = WaitForSingleObject(pi.hProcess, slice);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result != WAIT_TIMEOUT) break;     // failed/abandoned
        if (cancel_flag && cancel_flag->load()) {
            TerminateJobObject(job, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            cancelled = true;
            wait_result = WAIT_OBJECT_0;
            break;
        }
        if (remaining != INFINITE) {
            if (remaining <= slice) {
                wait_result = WAIT_TIMEOUT;
                break;
            }
            remaining -= slice;
        }
    }

    DWORD exit_code = 0;
    bool timed_out = false;
    if (wait_result == WAIT_TIMEOUT) {
        TerminateJobObject(job, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        timed_out = true;
        exit_code = 1;
    } else {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    }
    child_exited.store(true, std::memory_order_release);
    const auto t_child_exit = std::chrono::steady_clock::now();
    spdlog::info(
        "run_command[pid={}]: child exited (code={}, after {} ms{})",
        child_pid, exit_code,
        std::chrono::duration_cast<std::chrono::milliseconds>(t_child_exit - t_spawn).count(),
        cancelled ? ", cancelled" : (timed_out ? ", timed_out" : ""));

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Close the job *after* the reader's ReadFile loop has observed EOF.
    // TerminateJobObject above on timeout already closed the write handle
    // indirectly; on normal exit, write handle was closed earlier so the
    // reader is already winding down.
    reader.join();
    const auto t_reader_joined = std::chrono::steady_clock::now();
    const long long reader_join_delay_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            t_reader_joined - t_child_exit).count();
    // Anything more than a few hundred ms here is the pipe-drain hang bug --
    // see findings 7+8. Promote to warn for >1s, info otherwise.
    if (reader_join_delay_ms > 1000) {
        spdlog::warn(
            "run_command[pid={}]: reader join delayed {} ms after child exit "
            "(inherited-pipe leak symptom; {} bytes drained, last_err={})",
            child_pid, reader_join_delay_ms, reader_bytes_total.load(),
            reader_last_err.load());
    } else {
        spdlog::trace(
            "run_command[pid={}]: reader joined {} ms after child exit",
            child_pid, reader_join_delay_ms);
    }

    // Stop the heartbeat thread -- it may already be exiting due to
    // reader_done, but stop_flag ensures it wakes immediately rather than
    // waiting out its 30s tick.
    {
        std::lock_guard<std::mutex> lk(hb_mu);
        hb_stop = true;
    }
    hb_cv.notify_all();
    heartbeat.join();

    CloseHandle(read_pipe);
    CloseHandle(job);

    // Backstop hard cap on raw bytes before filtering. Was 8 KB, which
    // amputated useful build-log content before the filter could see it;
    // bumped to 1 MB so a typical "cmake --build" or full test suite still
    // fits in raw form and the output_filter / smart-truncate decides what
    // to return. Pathological multi-MB binary dumps still get clipped here
    // so the filter doesn't spend time chewing through them.
    constexpr size_t max_output = 1 * 1024 * 1024;
    if (output.size() > max_output) {
        output.resize(max_output);
        output += "\n[... raw output exceeded 1 MB; remainder discarded ...]";
    }

    // Full raw body to the trace log so the user can recover whatever the
    // filter elided. Cheap because trace is gated by -verbose.
    spdlog::trace("run_command: exit={} output={} bytes{}\n--- raw ---\n{}\n--- end raw ---",
                  exit_code, output.size(),
                  timed_out ? " (timed out)" : "", output);

    // Parse the output_filter_* args and apply. On a parse error we surface
    // a clean message instead of running with default truncation, since the
    // LLM probably meant something specific.
    OutputFilterSpec spec;
    std::string parse_err;
    if (!parse_output_filter_args(call.args, spec, parse_err)) {
        if (sink) sink->emit_sync_exited(static_cast<int>(exit_code), timed_out);
        return error_result("Error: " + parse_err);
    }
    int default_lines = 50;
    if (auto* wsp = ws.workspace())
        default_lines = wsp->config().agent.run_command_truncate_lines;
    std::string filtered = apply_output_filter(output, spec, default_lines);

    std::ostringstream content;
    if (cancelled) {
        content << "[cancelled by user]\n";
    } else if (timed_out) {
        content << "[TIMEOUT after " << timeout_ms << "ms]\n";
    }
    content << "[exit code: " << exit_code << "]\n";
    content << filtered;

    std::string result = content.str();

    if (sink) sink->emit_sync_exited(static_cast<int>(exit_code), timed_out);

    return {exit_code == 0 && !timed_out && !cancelled, result, result};
}

#else

ToolResult RunCommandTool::execute(const ToolCall& /*call*/, IWorkspaceServices& /*ws*/,
                                    const std::atomic<bool>* /*cancel_flag*/)
{
    // S5.Z task 5 -- prefix with the OS name so the activity-log line makes
    // it unambiguous *why* the call failed (Windows-only at v1; macOS / Linux
    // shell-out is a separate multi-week effort outside M5).
#if defined(__APPLE__)
    const char* os = "macos";
#elif defined(__linux__)
    const char* os = "linux";
#else
    const char* os = "unknown";
#endif
    return error_result(std::string("[platform: ") + os +
                        "] run_command is Windows-only in this build");
}

#endif

// -- Background process tools (S4.I) ---------------------------------------

namespace {

ToolResult no_registry()
{
    return error_result(
        "Error: background processes are not available -- this workspace has no process registry");
}

const char* status_str(BackgroundProcess::Status s) { return to_string(s); }

} // namespace

namespace {
// S5.A -- the four bg-process tools are gated together. Hide them from the
// per-turn manifest when the workspace has no registry (CLI / headless) OR
// when the capability bucket is off.
bool bg_capability_enabled(IWorkspaceServices& ws)
{
    auto* w = ws.workspace();
    return w ? w->config().capabilities.background_processes : true;
}
} // namespace

bool RunCommandBgTool::available(IWorkspaceServices& ws) const
{
    return ws.processes() != nullptr && bg_capability_enabled(ws);
}

std::string RunCommandBgTool::preview(const ToolCall& call) const
{
    return "Start in background: " + call.args.value("command", "");
}

ToolResult RunCommandBgTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                      const std::atomic<bool>* /*cancel_flag*/)
{
    auto* reg = ws.processes();
    if (!reg) return no_registry();

    std::string command = call.args.value("command", "");
    if (command.empty())
        return error_result("Error: 'command' parameter is required");

    int id = 0;
    try {
        id = reg->spawn(command);
    } catch (const std::exception& e) {
        return error_result(std::string("Error: spawn failed: ") + e.what());
    }

    nlohmann::json j = {
        {"process_id", id},
        {"command",    command},
        {"status",     "running"}
    };
    std::string content = j.dump();
    std::string display = "Started background process " + std::to_string(id) + ": " + command;
    return {true, content, display};
}

bool ReadProcessOutputTool::available(IWorkspaceServices& ws) const
{
    return ws.processes() != nullptr && bg_capability_enabled(ws);
}

std::string ReadProcessOutputTool::preview(const ToolCall& call) const
{
    int pid = call.args.value("process_id", 0);
    return "Read output of process " + std::to_string(pid);
}

ToolResult ReadProcessOutputTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                           const std::atomic<bool>* /*cancel_flag*/)
{
    auto* reg = ws.processes();
    if (!reg) return no_registry();

    if (!call.args.contains("process_id"))
        return error_result("Error: 'process_id' parameter is required");
    int id = call.args.value("process_id", 0);

    std::optional<std::size_t> since;
    if (call.args.contains("since_offset") && call.args["since_offset"].is_number_integer()) {
        long long v = call.args["since_offset"].get<long long>();
        if (v < 0) v = 0;
        since = static_cast<std::size_t>(v);
    }

    auto r = reg->read_output(id, since);
    if (!r) return error_result("Error: unknown process_id " + std::to_string(id));

    // Apply the same output_filter family run_command supports. Defaults
    // match the workspace knob; explicit args win. Trace-log the full
    // pre-filter window so the user can recover it from .locus/locus.log.
    OutputFilterSpec spec;
    std::string parse_err;
    if (!parse_output_filter_args(call.args, spec, parse_err))
        return error_result("Error: " + parse_err);
    int default_lines = 50;
    if (auto* wsp = ws.workspace())
        default_lines = wsp->config().agent.run_command_truncate_lines;
    spdlog::trace("read_process_output: id={} bytes={} next_offset={} dropped={}\n"
                  "--- raw ---\n{}\n--- end raw ---",
                  id, r->data.size(), r->next_offset, r->dropped_before_window,
                  r->data);
    std::string filtered_data = apply_output_filter(r->data, spec, default_lines);

    std::ostringstream display;
    display << "[process " << id << " | " << status_str(r->status);
    if (r->status != BackgroundProcess::Status::running)
        display << " exit=" << r->exit_code;
    display << " | next_offset=" << r->next_offset;
    if (r->dropped_before_window > 0)
        display << " | dropped=" << r->dropped_before_window << "B";
    display << "]\n" << filtered_data;

    nlohmann::json j = {
        {"process_id",            id},
        {"status",                status_str(r->status)},
        {"next_offset",           r->next_offset},
        {"dropped_before_window", r->dropped_before_window},
        {"data",                  filtered_data},
    };
    if (r->status != BackgroundProcess::Status::running)
        j["exit_code"] = r->exit_code;

    return {true, j.dump(), display.str()};
}

bool StopProcessTool::available(IWorkspaceServices& ws) const
{
    return ws.processes() != nullptr && bg_capability_enabled(ws);
}

std::string StopProcessTool::preview(const ToolCall& call) const
{
    int pid = call.args.value("process_id", 0);
    return "Stop process " + std::to_string(pid);
}

ToolResult StopProcessTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                     const std::atomic<bool>* /*cancel_flag*/)
{
    auto* reg = ws.processes();
    if (!reg) return no_registry();

    if (!call.args.contains("process_id"))
        return error_result("Error: 'process_id' parameter is required");
    int id = call.args.value("process_id", 0);

    if (!reg->stop(id))
        return error_result("Error: unknown process_id " + std::to_string(id));

    std::string msg = "Process " + std::to_string(id) + " terminated";
    return {true, msg, msg};
}

bool ListProcessesTool::available(IWorkspaceServices& ws) const
{
    return ws.processes() != nullptr && bg_capability_enabled(ws);
}

ToolResult ListProcessesTool::execute(const ToolCall& /*call*/, IWorkspaceServices& ws,
                                       const std::atomic<bool>* /*cancel_flag*/)
{
    auto* reg = ws.processes();
    if (!reg) return no_registry();

    auto entries = reg->list();
    nlohmann::json arr = nlohmann::json::array();
    std::ostringstream display;

    if (entries.empty()) {
        display << "(no background processes)";
    } else {
        display << "id  status   exit  pending  command\n";
        for (auto& e : entries) {
            display << e.id << "  " << status_str(e.status)
                    << "  " << e.exit_code
                    << "  " << e.bytes_pending
                    << "  " << e.command << "\n";
            arr.push_back({
                {"process_id",      e.id},
                {"command",         e.command},
                {"started_at_unix", e.started_at_unix},
                {"status",          status_str(e.status)},
                {"exit_code",       e.exit_code},
                {"bytes_pending",   e.bytes_pending},
            });
        }
    }
    return {true, arr.dump(), display.str()};
}

} // namespace locus

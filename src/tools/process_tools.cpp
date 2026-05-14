#include "tools/process_tools.h"
#include "tools/process_sink.h"
#include "tools/shared.h"

#include "core/workspace.h"
#include "core/workspace_services.h"
#include "process_registry.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
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

ToolResult RunCommandTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string command = call.args.value("command", "");
    // Default 30 minutes — matches Pi's bash tool. Long-enough builds, test
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
    // on cmd would orphan ping — its inherited write handle would keep our
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
        // Failed to put the child in the job — kill it before it spawns
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

    // Drain the pipe on a background thread so the terminal panel (if any)
    // sees chunks live rather than as one wall of text after exit. We still
    // capture the full output for the agent's tool result. WaitForSingleObject
    // returns when the process exits; the job-kill on timeout closes the
    // write end so the reader thread observes EOF and joins.
    std::string  output;
    std::mutex   output_mu;
    std::thread reader([&]{
        char buf[4096];
        DWORD bytes_read = 0;
        while (ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
            if (sink) sink->emit_sync_chunk(buf, bytes_read);
            std::lock_guard l(output_mu);
            output.append(buf, bytes_read);
        }
    });

    DWORD wait_result = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));

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

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Close the job *after* the reader's ReadFile loop has observed EOF.
    // TerminateJobObject above on timeout already closed the write handle
    // indirectly; on normal exit, write handle was closed earlier so the
    // reader is already winding down.
    reader.join();
    CloseHandle(read_pipe);
    CloseHandle(job);

    constexpr size_t max_output = 8000;
    if (output.size() > max_output) {
        output.resize(max_output);
        output += "\n... (output truncated)";
    }

    std::ostringstream content;
    if (timed_out)
        content << "[TIMEOUT after " << timeout_ms << "ms]\n";
    content << "[exit code: " << exit_code << "]\n";
    content << output;

    std::string result = content.str();

    spdlog::trace("run_command: exit={} output={} bytes{}", exit_code,
                  output.size(), timed_out ? " (timed out)" : "");

    if (sink) sink->emit_sync_exited(static_cast<int>(exit_code), timed_out);

    return {exit_code == 0 && !timed_out, result, result};
}

#else

ToolResult RunCommandTool::execute(const ToolCall& /*call*/, IWorkspaceServices& /*ws*/)
{
    return error_result("Error: run_command not implemented on this platform");
}

#endif

// -- Background process tools (S4.I) ---------------------------------------

namespace {

ToolResult no_registry()
{
    return error_result(
        "Error: background processes are not available — this workspace has no process registry");
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

ToolResult RunCommandBgTool::execute(const ToolCall& call, IWorkspaceServices& ws)
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

ToolResult ReadProcessOutputTool::execute(const ToolCall& call, IWorkspaceServices& ws)
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

    std::ostringstream display;
    display << "[process " << id << " | " << status_str(r->status);
    if (r->status != BackgroundProcess::Status::running)
        display << " exit=" << r->exit_code;
    display << " | next_offset=" << r->next_offset;
    if (r->dropped_before_window > 0)
        display << " | dropped=" << r->dropped_before_window << "B";
    display << "]\n" << r->data;

    nlohmann::json j = {
        {"process_id",            id},
        {"status",                status_str(r->status)},
        {"next_offset",           r->next_offset},
        {"dropped_before_window", r->dropped_before_window},
        {"data",                  r->data},
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

ToolResult StopProcessTool::execute(const ToolCall& call, IWorkspaceServices& ws)
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

ToolResult ListProcessesTool::execute(const ToolCall& /*call*/, IWorkspaceServices& ws)
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

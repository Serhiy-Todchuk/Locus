#include "tools/process_tools.h"
#include "tools/shared.h"

#include "core/workspace_services.h"

#include <spdlog/spdlog.h>

#include <sstream>
#include <string>

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
    int timeout_ms = call.args.value("timeout_ms", 30000);

    if (command.empty())
        return error_result("Error: 'command' parameter is required");

    spdlog::trace("run_command: '{}'", command);

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

    // Wait for the child with timeout. On timeout, terminating the *job* kills
    // every process spawned under it (including grandchildren), so the pipe's
    // write end finally closes and the ReadFile loop below can return.
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

    std::string output;
    char buf[4096];
    DWORD bytes_read;
    while (ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        output.append(buf, bytes_read);
    }
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

    return {exit_code == 0 && !timed_out, result, result};
}

#else

ToolResult RunCommandTool::execute(const ToolCall& /*call*/, IWorkspaceServices& /*ws*/)
{
    return error_result("Error: run_command not implemented on this platform");
}

#endif

} // namespace locus

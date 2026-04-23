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

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError  = write_pipe;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    std::string cmd_line = "cmd /c " + command;

    BOOL created = CreateProcessA(
        nullptr,
        cmd_line.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        ws.root().string().c_str(),
        &si, &pi
    );

    CloseHandle(write_pipe);

    if (!created) {
        CloseHandle(read_pipe);
        return error_result("Error: failed to create process (error " +
                            std::to_string(GetLastError()) + ")");
    }

    // Wait for process with timeout FIRST, then drain the pipe. This ensures
    // the timeout actually fires for long-running commands (ReadFile on a pipe
    // blocks until the child's write end closes).
    DWORD wait_result = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));

    DWORD exit_code = 0;
    bool timed_out = false;
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
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

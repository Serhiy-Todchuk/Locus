#include "mcp/stdio_transport.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace locus {

namespace fs = std::filesystem;

StdioTransport::StdioTransport() = default;

StdioTransport::~StdioTransport()
{
    terminate();

    if (reader_.joinable()) reader_.join();

#ifdef _WIN32
    if (stdin_w_)  { CloseHandle(static_cast<HANDLE>(stdin_w_));  stdin_w_  = nullptr; }
    if (stdout_r_) { CloseHandle(static_cast<HANDLE>(stdout_r_)); stdout_r_ = nullptr; }
    if (process_)  { CloseHandle(static_cast<HANDLE>(process_));  process_  = nullptr; }
    if (job_)      { CloseHandle(static_cast<HANDLE>(job_));      job_      = nullptr; }
#endif
}

#ifdef _WIN32

namespace {

// Quote one argv token per the convention CommandLineToArgvW reverses.
// Implements the algorithm described in:
//   https://learn.microsoft.com/en-us/archive/blogs/twistylittlepassagesallalike/
// Required for arguments containing spaces, quotes, or trailing backslashes.
void append_arg_quoted(std::string& out, const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        out.append(arg);
        return;
    }
    out.push_back('"');
    for (size_t i = 0; i < arg.size(); ) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') { ++backslashes; ++i; }

        if (i == arg.size()) {
            // Trailing backslashes: double each so the closing quote isn't escaped.
            out.append(backslashes * 2, '\\');
        } else if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            ++i;
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
            ++i;
        }
    }
    out.push_back('"');
}

std::string build_command_line(const std::string& command,
                               const std::vector<std::string>& args)
{
    std::string out;
    append_arg_quoted(out, command);
    for (auto& a : args) {
        out.push_back(' ');
        append_arg_quoted(out, a);
    }
    return out;
}

// Build a Win32 environment block from the parent's environment with the
// caller's overrides merged on top. Returns null-terminated KEY=VALUE pairs
// followed by a trailing null. Case-insensitive comparison on Windows.
std::string build_env_block(const std::map<std::string, std::string>& overrides)
{
    std::map<std::string, std::string, std::less<>> merged;
    auto upper = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
        return s;
    };

    LPCH parent = GetEnvironmentStringsA();
    if (parent) {
        for (LPCH p = parent; *p; ) {
            std::string entry(p);
            p += entry.size() + 1;
            auto eq = entry.find('=');
            if (eq == std::string::npos || eq == 0) continue;  // skip drive markers like "=C:=..."
            std::string key = upper(entry.substr(0, eq));
            std::string val = entry.substr(eq + 1);
            merged[key] = key + "=" + val;
        }
        FreeEnvironmentStringsA(parent);
    }
    for (auto& [k, v] : overrides) {
        std::string ku = upper(k);
        merged[ku] = k + "=" + v;
    }

    std::string block;
    for (auto& [k, line] : merged) {
        block.append(line);
        block.push_back('\0');
    }
    block.push_back('\0');  // terminator
    return block;
}

} // namespace

void StdioTransport::spawn(const std::string& command,
                            const std::vector<std::string>& args,
                            const std::map<std::string, std::string>& env_overrides,
                            const fs::path& cwd)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_r = nullptr, child_stdin_w = nullptr;
    HANDLE child_stdout_r = nullptr, child_stdout_w = nullptr;

    if (!CreatePipe(&child_stdin_r, &child_stdin_w, &sa, 0))
        throw std::runtime_error("CreatePipe (stdin) failed");
    SetHandleInformation(child_stdin_w, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&child_stdout_r, &child_stdout_w, &sa, 0)) {
        CloseHandle(child_stdin_r);
        CloseHandle(child_stdin_w);
        throw std::runtime_error("CreatePipe (stdout) failed");
    }
    SetHandleInformation(child_stdout_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (!job) {
        CloseHandle(child_stdin_r);  CloseHandle(child_stdin_w);
        CloseHandle(child_stdout_r); CloseHandle(child_stdout_w);
        throw std::runtime_error("CreateJobObject failed");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
    jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jli, sizeof(jli));

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = child_stdin_r;
    si.hStdOutput = child_stdout_w;
    // Inherit our stderr so the server's diagnostics surface in our log.
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    std::string cmd_line = build_command_line(command, args);
    std::string env_block = build_env_block(env_overrides);
    std::string cwd_str   = cwd.empty() ? std::string{} : cwd.string();

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessA(
        nullptr,
        cmd_line.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        env_block.data(),
        cwd_str.empty() ? nullptr : cwd_str.c_str(),
        &si, &pi);

    // Close the child's ends; we don't need them in the parent.
    CloseHandle(child_stdin_r);
    CloseHandle(child_stdout_w);

    if (!created) {
        DWORD err = GetLastError();
        CloseHandle(child_stdin_w);
        CloseHandle(child_stdout_r);
        CloseHandle(job);
        throw std::runtime_error("CreateProcess failed (" + std::to_string(err) +
                                 ") cmd: " + cmd_line);
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(child_stdin_w);
        CloseHandle(child_stdout_r);
        CloseHandle(job);
        throw std::runtime_error("AssignProcessToJobObject failed (" +
                                 std::to_string(err) + ")");
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    process_  = pi.hProcess;
    job_      = job;
    stdin_w_  = child_stdin_w;
    stdout_r_ = child_stdout_r;

    running_  = true;
    reader_   = std::thread(&StdioTransport::reader_loop, this);

    spdlog::info("StdioTransport: spawned PID {} cmd: {}",
                 GetProcessId(pi.hProcess), cmd_line);
}

bool StdioTransport::send_line(const std::string& json_line)
{
    if (!running_.load()) return false;

    std::string buf;
    buf.reserve(json_line.size() + 1);
    buf.append(json_line);
    if (buf.empty() || buf.back() != '\n') buf.push_back('\n');

    std::lock_guard l(write_mu_);
    HANDLE h = static_cast<HANDLE>(stdin_w_);
    if (!h) return false;

    DWORD written = 0;
    DWORD remaining = static_cast<DWORD>(buf.size());
    const char* p = buf.data();
    while (remaining > 0) {
        if (!WriteFile(h, p, remaining, &written, nullptr) || written == 0) {
            spdlog::warn("StdioTransport: write failed, child likely exited");
            return false;
        }
        p += written;
        remaining -= written;
    }
    return true;
}

void StdioTransport::terminate()
{
    if (!running_.exchange(false)) return;

    HANDLE job = static_cast<HANDLE>(job_);
    if (job) TerminateJobObject(job, 1);

    // Closing our write end signals EOF to the child; closing the read end
    // ensures the reader thread's blocking ReadFile returns quickly.
    HANDLE w = static_cast<HANDLE>(stdin_w_);
    if (w) {
        // Don't actually close yet; the dtor still owns the handle. But a
        // CancelIoEx wakes any pending write.
        CancelIoEx(w, nullptr);
    }
}

void StdioTransport::reader_loop()
{
    HANDLE h = static_cast<HANDLE>(stdout_r_);
    if (!h) {
        running_ = false;
        if (closed_cb_) closed_cb_();
        return;
    }

    std::string carry;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        carry.append(buf, n);

        std::size_t start = 0;
        while (true) {
            std::size_t nl = carry.find('\n', start);
            if (nl == std::string::npos) break;

            std::size_t end = nl;
            // Strip trailing \r so CRLF servers don't ship a stray byte.
            if (end > start && carry[end - 1] == '\r') --end;

            if (end > start && msg_cb_) {
                try {
                    msg_cb_(carry.substr(start, end - start));
                } catch (const std::exception& e) {
                    spdlog::warn("StdioTransport: message handler threw: {}", e.what());
                }
            }
            start = nl + 1;
        }
        if (start > 0) carry.erase(0, start);
    }

    running_ = false;
    if (closed_cb_) {
        try { closed_cb_(); }
        catch (const std::exception& e) {
            spdlog::warn("StdioTransport: closed handler threw: {}", e.what());
        }
    }
}

#else // !_WIN32

void StdioTransport::spawn(const std::string&, const std::vector<std::string>&,
                            const std::map<std::string, std::string>&,
                            const fs::path&)
{
    throw std::runtime_error("MCP stdio transport not implemented on this platform");
}
bool StdioTransport::send_line(const std::string&) { return false; }
void StdioTransport::terminate() { running_ = false; }
void StdioTransport::reader_loop() {}

#endif

} // namespace locus

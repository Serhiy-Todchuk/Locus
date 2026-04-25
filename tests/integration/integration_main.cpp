#include "harness_fixture.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <set>
#include <string>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

namespace {

#ifdef _WIN32

// Sentinel passed to the self-relaunched child so the child skips its own
// self-relaunch (avoiding an infinite fork loop).
constexpr const char* k_console_child_flag = "--console-child";

// True if stderr is bound to a real character device (a console) — as
// opposed to a pipe or disk file. Used to detect "no visible console"
// launches (IDE Run, agent subprocess, redirected pipe) and trigger a
// self-relaunch that forces CREATE_NEW_CONSOLE.
bool stderr_is_real_console()
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    return GetFileType(h) == FILE_TYPE_CHAR;
}

// Configure the console we're attached to: UTF-8 code pages, VT processing
// for color, larger scrollback, friendly title. Safe to call on any
// already-attached console (whether inherited or just created).
void configure_console()
{
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(h_out, &mode))
            SetConsoleMode(h_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (GetConsoleScreenBufferInfo(h_out, &csbi)) {
            COORD size = csbi.dwSize;
            if (size.Y < 5000) {
                size.Y = 5000;
                SetConsoleScreenBufferSize(h_out, size);
            }
        }
    }

    SetConsoleTitleW(L"Locus Integration Tests");
}

std::wstring widen(const char* s)
{
    if (!s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// Quote a single argv element for CreateProcessW's flat command line.
// Mirrors the standard MSVCRT parsing rules (CommandLineToArgvW).
std::wstring quote_arg(const std::wstring& a)
{
    const bool needs_quote =
        a.empty() || a.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quote) return a;

    std::wstring out;
    out.reserve(a.size() + 2);
    out.push_back(L'"');
    int backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(static_cast<size_t>(backslashes * 2 + 1), L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            if (backslashes) {
                out.append(static_cast<size_t>(backslashes), L'\\');
                backslashes = 0;
            }
            out.push_back(c);
        }
    }
    out.append(static_cast<size_t>(backslashes * 2), L'\\');
    out.push_back(L'"');
    return out;
}

// Re-launch this process in a dedicated console window, wait for it, return
// its exit code. Used when -console was requested but our stderr isn't a
// real console (IDE / subprocess / pipe launch) — AllocConsole can't help
// in that case (ACCESS_DENIED under some subprocess shells), but Windows
// always honors CREATE_NEW_CONSOLE for a freshly-spawned child.
int relaunch_in_new_console(int argc, char** argv)
{
    wchar_t exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        std::fprintf(stderr, "[integration] GetModuleFileNameW failed\n");
        return 1;
    }

    std::wstring cmdline = quote_arg(exe_path);
    for (int i = 1; i < argc; ++i) {
        cmdline.push_back(L' ');
        cmdline += quote_arg(widen(argv[i]));
    }
    // Sentinel so the child skips its own self-relaunch branch.
    cmdline.push_back(L' ');
    cmdline += quote_arg(widen(k_console_child_flag));

    STARTUPINFOW          si{};
    PROCESS_INFORMATION   pi{};
    si.cb = sizeof(si);

    BOOL ok = CreateProcessW(
        exe_path, cmdline.data(),
        nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE,
        nullptr, nullptr,
        &si, &pi);
    if (!ok) {
        std::fprintf(stderr,
            "[integration] CreateProcessW(CREATE_NEW_CONSOLE) failed (err=%lu)\n",
            GetLastError());
        return 1;
    }

    std::fprintf(stderr,
        "[integration] -console: spawned child in new window (pid=%lu). "
        "Waiting for tests to finish...\n",
        pi.dwProcessId);

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
}
#endif

// Catch2 listener that:
//   * resets the harness's conversation history at most ONCE per area, where
//     "area" is the source file of the TEST_CASE (each test_int_<topic>.cpp
//     is one area: [search], [outline], ...). Catch2's default test order
//     can interleave files, so a "reset whenever the file changes" strategy
//     thrashes -- here we track areas already reset and skip subsequent
//     re-entries. Per-case reset was correct but slow (every case re-paid
//     the system prompt + tool-manifest tokens); per-area reset keeps the
//     full-suite conversation from drifting toward Gemma 4 E4B's 16k ceiling
//     while paying the reset cost at most N-areas times per run;
//   * tears down the shared harness once all tests have finished running.
//
// Reset uses the non-constructing peek so the first area boundary (before
// any test body has called harness()) is a no-op rather than forcing LM
// Studio init.
class IntegrationSessionListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& info) override
    {
        const char* file = info.lineInfo.file ? info.lineInfo.file : "";
        if (!areas_reset_.insert(file).second) return;  // already reset this area

        if (auto* h = locus::integration::IntegrationHarness::shared_if_alive()) {
            h->agent().reset_conversation();
        }
    }

    void testRunEnded(const Catch::TestRunStats& /*stats*/) override
    {
        locus::integration::IntegrationHarness::shutdown_shared();
    }

private:
    std::set<std::string> areas_reset_;
};

} // namespace

CATCH_REGISTER_LISTENER(IntegrationSessionListener)

// Custom main — Catch2 args plus our -console flag. -console mirrors all
// trace-level log output to stderr so long runs are observable live instead
// of only landing in .locus/integration_test.log.
int main(int argc, char** argv)
{
    std::vector<char*> forwarded;
    forwarded.reserve(argc);
    forwarded.push_back(argv[0]);

    bool console_logging = false;
    bool console_child   = false;  // we're the relaunched child; don't re-fork
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-console") == 0) {
            console_logging = true;
#ifdef _WIN32
        } else if (std::strcmp(argv[i], k_console_child_flag) == 0) {
            console_child = true;  // consumed — not forwarded to Catch2
#endif
        } else {
            forwarded.push_back(argv[i]);
        }
    }

#ifdef _WIN32
    // If the user asked for a console but we don't have a real one (IDE Run,
    // subprocess pipe, agent-invoked), self-relaunch with CREATE_NEW_CONSOLE
    // so a dedicated window pops up. The child carries --console-child and
    // skips this branch.
    if (console_logging && !console_child && !stderr_is_real_console()) {
        return relaunch_in_new_console(argc, argv);
    }
    if (console_logging) {
        configure_console();
    }
#endif

    if (console_logging) {
        std::cerr << "[integration] -console: streaming trace log to this console\n";
    }

    locus::integration::set_console_logging(console_logging);

    int forwarded_argc = static_cast<int>(forwarded.size());
    return Catch::Session().run(forwarded_argc, forwarded.data());
}

#include "auto_commit.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace locus {

namespace fs = std::filesystem;

// -- Subject trimming ---------------------------------------------------------

std::string make_commit_subject(const std::string& text, std::size_t max_chars)
{
    // First non-empty line.
    std::string subject;
    {
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            // Strip CR.
            while (!line.empty() && line.back() == '\r') line.pop_back();
            // Skip blank lines until we find content.
            std::string stripped = line;
            while (!stripped.empty()
                   && (stripped.front() == ' ' || stripped.front() == '\t'))
                stripped.erase(0, 1);
            if (!stripped.empty()) {
                subject = stripped;
                break;
            }
        }
    }

    if (subject.empty()) return subject;

    // Trim trailing whitespace.
    while (!subject.empty()
           && (subject.back() == ' ' || subject.back() == '\t'))
        subject.pop_back();

    if (subject.size() > max_chars) {
        // Reserve 3 chars for the "..." marker.
        std::size_t cut = (max_chars > 3) ? max_chars - 3 : max_chars;
        subject.resize(cut);
        // Trim trailing whitespace again so "foo " doesn't become "foo ...".
        while (!subject.empty()
               && (subject.back() == ' ' || subject.back() == '\t'))
            subject.pop_back();
        subject += "...";
    }

    return subject;
}

// -- Subprocess runner --------------------------------------------------------

namespace {

struct CommandResult {
    bool        ran = false;       // false if CreateProcess itself failed
    int         exit_code = 0;
    std::string output;            // combined stdout + stderr
};

#ifdef _WIN32

// Run `cmd_line` in `cwd` synchronously. Captures stdout+stderr together,
// returns exit code. Used only for short-lived `git ...` invocations -- no
// timeout, no Job Object overhead (git's all sync work, no grandchildren we
// need to police).
CommandResult run_sync(const std::string& cmd_line, const fs::path& cwd)
{
    CommandResult cr;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return cr;

    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError  = write_pipe;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = cmd_line;

    BOOL created = CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd.string().c_str(),
        &si, &pi
    );

    CloseHandle(write_pipe);

    if (!created) {
        CloseHandle(read_pipe);
        spdlog::trace("auto_commit: CreateProcess failed for '{}' (err {})",
                      cmd_line, GetLastError());
        return cr;
    }

    cr.ran = true;

    char buf[4096];
    DWORD bytes_read = 0;
    while (ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr)
           && bytes_read > 0) {
        cr.output.append(buf, bytes_read);
    }
    CloseHandle(read_pipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    cr.exit_code = static_cast<int>(exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return cr;
}

#else

CommandResult run_sync(const std::string& cmd_line, const fs::path& cwd)
{
    CommandResult cr;
    // Non-Windows: simple popen path. Locus is Windows-first; this branch
    // exists so the file compiles in cross-platform CI builds.
    fs::path prev = fs::current_path();
    std::error_code ec;
    fs::current_path(cwd, ec);
    if (ec) return cr;
    std::string full = cmd_line + " 2>&1";
    FILE* p = popen(full.c_str(), "r");
    fs::current_path(prev, ec);
    if (!p) return cr;
    cr.ran = true;
    char buf[4096];
    while (auto n = std::fread(buf, 1, sizeof(buf), p))
        cr.output.append(buf, n);
    int rc = pclose(p);
    cr.exit_code = WEXITSTATUS(rc);
    return cr;
}

#endif

void rstrip(std::string& s)
{
    while (!s.empty()
           && (s.back() == '\n' || s.back() == '\r'
               || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
}

bool branch_exists(const fs::path& root, const std::string& branch)
{
    auto r = run_sync("git rev-parse --verify --quiet refs/heads/" + branch, root);
    return r.ran && r.exit_code == 0;
}

bool current_branch(const fs::path& root, std::string& out)
{
    auto r = run_sync("git rev-parse --abbrev-ref HEAD", root);
    if (!r.ran || r.exit_code != 0) return false;
    out = r.output;
    rstrip(out);
    return !out.empty();
}

} // namespace

// -- Public entry point ------------------------------------------------------

AutoCommitResult auto_commit_after_turn(const fs::path&    workspace_root,
                                        const std::string& commit_prefix,
                                        const std::string& commit_branch,
                                        const std::string& assistant_message)
{
    AutoCommitResult res;

    std::error_code ec;
    if (!fs::is_directory(workspace_root / ".git", ec)) {
        res.skipped = true;
        res.success = true;          // not a git repo is not a failure
        spdlog::trace("auto_commit: not a git repo, skipping");
        return res;
    }

    std::string subject = make_commit_subject(assistant_message);
    if (subject.empty()) {
        res.skipped = true;
        res.success = true;
        spdlog::trace("auto_commit: no commit subject, skipping");
        return res;
    }

    // Optional branch switch. Done BEFORE `git add -A` -- otherwise the stage
    // would include changes for the branch we're about to leave.
    std::string target_branch = commit_branch;
    if (!target_branch.empty()) {
        bool exists = branch_exists(workspace_root, target_branch);
        if (!exists) {
            auto cr = run_sync("git branch " + target_branch, workspace_root);
            if (!cr.ran || cr.exit_code != 0) {
                res.success = false;
                res.error   = "Failed to create branch '" + target_branch
                              + "': " + cr.output;
                return res;
            }
            spdlog::warn("auto_commit: created branch '{}' (was missing)",
                         target_branch);
        }
        std::string cur;
        if (current_branch(workspace_root, cur) && cur != target_branch) {
            auto cr = run_sync("git checkout " + target_branch, workspace_root);
            if (!cr.ran || cr.exit_code != 0) {
                res.success = false;
                res.error   = "Failed to checkout '" + target_branch
                              + "': " + cr.output;
                return res;
            }
        }
    }

    // Stage everything and commit. We deliberately use `git add -A` (matching
    // the spec); the user opts in to the auto-commit feature and gets the
    // exact behaviour the docs promise.
    {
        auto cr = run_sync("git add -A", workspace_root);
        if (!cr.ran || cr.exit_code != 0) {
            res.success = false;
            res.error   = "git add failed: " + cr.output;
            return res;
        }
    }

    {
        // Quote the message via a temp file so the commit subject can contain
        // any character without escaping headaches on cmd.exe. `git commit
        // -F <file>` reads the message verbatim.
        fs::path tmp = workspace_root / ".locus" / "auto_commit_msg.tmp";
        std::error_code _;
        fs::create_directories(tmp.parent_path(), _);
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << commit_prefix << subject;
        }

        std::string cmd = "git commit -F \"" + tmp.string() + "\"";
        auto cr = run_sync(cmd, workspace_root);
        std::error_code _e;
        fs::remove(tmp, _e);

        if (!cr.ran || cr.exit_code != 0) {
            // The most common failure -- "nothing to commit" -- is harmless;
            // we already gated on the change_tracker, but a hooked workspace
            // can still produce empty stages (e.g. .gitignore scoped out
            // every change). Not an error from the user's perspective.
            std::string out_lower = cr.output;
            std::transform(out_lower.begin(), out_lower.end(), out_lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (out_lower.find("nothing to commit") != std::string::npos
                || out_lower.find("nothing added to commit") != std::string::npos
                || out_lower.find("no changes added to commit") != std::string::npos)
            {
                res.skipped = true;
                res.success = true;
                spdlog::trace("auto_commit: nothing to commit");
                return res;
            }
            res.success = false;
            res.error   = "git commit failed: " + cr.output;
            return res;
        }
    }

    {
        auto cr = run_sync("git rev-parse --short HEAD", workspace_root);
        if (cr.ran && cr.exit_code == 0) {
            res.short_sha = cr.output;
            rstrip(res.short_sha);
        }
    }

    std::string actual_branch;
    if (current_branch(workspace_root, actual_branch))
        res.branch = actual_branch;

    res.success = true;
    spdlog::info("auto_commit: created commit {} on branch '{}'",
                 res.short_sha, res.branch);
    return res;
}

} // namespace locus

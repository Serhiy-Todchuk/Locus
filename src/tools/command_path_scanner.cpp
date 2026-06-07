#include "command_path_scanner.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace locus::tools {

namespace fs = std::filesystem;

namespace {

// Shell-style tokenizer: whitespace separates tokens, single and double
// quotes group an arbitrary run into one token (quotes are removed from
// the result). Backslash-as-escape inside double quotes is intentionally
// NOT honoured -- the agent emits commands for cmd.exe, where `\` is a
// path separator, not an escape char.
std::vector<std::string> tokenize(std::string_view cmd)
{
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    for (char c : cmd) {
        if (quote) {
            if (c == quote) {
                quote = 0;
                continue;  // strip the closing quote
            }
            cur += c;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;  // strip the opening quote
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool starts_with_drive_letter(std::string_view tok)
{
    return tok.size() >= 3
        && std::isalpha(static_cast<unsigned char>(tok[0]))
        && tok[1] == ':'
        && (tok[2] == '\\' || tok[2] == '/');
}

bool starts_with_unc(std::string_view tok)
{
    return tok.size() >= 2 && (
        (tok[0] == '\\' && tok[1] == '\\') ||
        (tok[0] == '/'  && tok[1] == '/'));
}

bool starts_with_unix_abs(std::string_view tok)
{
    return tok.size() >= 1 && tok[0] == '/' && !starts_with_unc(tok);
}

// Windows cmd / PowerShell switches start with `/<letter>` and don't contain
// any further path separator (`cd /d`, `cmd /c`, `xcopy /e`, `dir /a:rh`,
// `findstr /v`, etc.). Real Unix-style paths almost always have multiple
// segments (`/etc/passwd`, `/usr/bin/env`), so a slash-prefixed token with
// no second separator is overwhelmingly a switch in a Windows shell context.
// 16-char cap keeps the false-negative window small for any genuinely
// single-segment Unix path that does sneak in (`/etc`, `/var`, `/opt`).
bool looks_like_cmd_switch(std::string_view tok)
{
    if (tok.size() < 2 || tok[0] != '/') return false;
    if (tok.size() > 16) return false;
    char c = tok[1];
    if (!std::isalpha(static_cast<unsigned char>(c)) && c != '?') return false;
    for (size_t i = 2; i < tok.size(); ++i) {
        if (tok[i] == '/' || tok[i] == '\\') return false;
    }
    return true;
}

bool contains_parent_segment(std::string_view tok)
{
    // "..\foo", "../foo", "foo/../bar", "..\..\x". Reject the bare token
    // "...something" though -- only flag when ".." is a path-segment
    // (preceded by start-of-token, '\', or '/' and followed by end,
    // '\', or '/').
    for (size_t i = 0; i + 1 < tok.size(); ++i) {
        if (tok[i] != '.' || tok[i + 1] != '.') continue;
        bool left_ok  = (i == 0) || tok[i - 1] == '/' || tok[i - 1] == '\\';
        bool right_ok = (i + 2 == tok.size())
                     || tok[i + 2] == '/' || tok[i + 2] == '\\';
        if (left_ok && right_ok) return true;
    }
    return false;
}

bool starts_with_env_or_home(std::string_view tok)
{
    if (tok.empty()) return false;
    char c = tok[0];
    if (c == '$' || c == '~') return true;
    // %FOO% needs a closing percent (otherwise it's just a `%` literal,
    // which cmd.exe ignores).
    if (c == '%') {
        for (size_t i = 1; i < tok.size(); ++i) {
            if (tok[i] == '%') return i > 1;  // %FOO% but not %%
        }
    }
    return false;
}

bool is_under(const fs::path& candidate, const fs::path& root)
{
    auto cs = candidate.string();
    auto rs = root.string();
    if (cs.size() < rs.size()) return false;
    return cs.compare(0, rs.size(), rs) == 0;
}

} // namespace

std::vector<std::string> scan_outside_workspace_paths(
    std::string_view command,
    const fs::path& workspace_root)
{
    std::vector<std::string> flagged;
    if (workspace_root.empty()) return flagged;

    std::error_code ec;
    fs::path root = fs::weakly_canonical(workspace_root, ec);
    if (ec) root = workspace_root;

    for (const auto& tok : tokenize(command)) {
        // Strip a trailing ',' / ';' / ')' so a token like
        // `C:\Users\me\foo;` doesn't escape canonicalization.
        std::string t = tok;
        while (!t.empty() && (t.back() == ',' || t.back() == ';' || t.back() == ')')) {
            t.pop_back();
        }
        if (t.empty()) continue;

        // `cd /d D:\foo`, `cmd /c ...`, etc. -- the `/d` token would
        // otherwise hit the unix-abs branch and canonicalize to `<drive>:\d`,
        // which is outside the workspace.
        if (looks_like_cmd_switch(t)) continue;

        bool path_shape = starts_with_drive_letter(t)
                       || starts_with_unc(t)
                       || starts_with_unix_abs(t)
                       || contains_parent_segment(t);
        bool env_shape  = starts_with_env_or_home(t);

        if (!path_shape && !env_shape) continue;

        if (env_shape) {
            // Don't try to expand. Almost always points outside the
            // workspace; flagging here also makes the safety prompt
            // catch redirected output to $HOME / %USERPROFILE%.
            if (std::find(flagged.begin(), flagged.end(), tok) == flagged.end())
                flagged.push_back(tok);
            continue;
        }

        fs::path candidate;
        if (starts_with_drive_letter(t) || starts_with_unc(t)
            || starts_with_unix_abs(t))
        {
            candidate = fs::weakly_canonical(t, ec);
            if (ec) candidate = t;
        } else {
            // Parent-relative path -- canonicalize against root. Normalize '\'
            // to '/' first: off-Windows, std::filesystem::path treats '\' as an
            // ordinary filename char, so "..\..\x" would resolve to one odd
            // filename *inside* root instead of escaping it. This scanner is a
            // cross-platform tripwire -- a Windows-style parent traversal in a
            // command string is suspicious on any host. The reported token (tok)
            // keeps its original spelling; only the resolution is normalized.
            std::string norm = t;
            std::replace(norm.begin(), norm.end(), '\\', '/');
            candidate = fs::weakly_canonical(root / norm, ec);
            if (ec) candidate = root / norm;
        }

        if (!is_under(candidate, root)) {
            if (std::find(flagged.begin(), flagged.end(), tok) == flagged.end())
                flagged.push_back(tok);
        }
    }
    return flagged;
}

} // namespace locus::tools

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace locus::tools {

// S4.V Task 5 -- best-effort scanner that flags shell command strings whose
// tokens reference filesystem locations outside the workspace root.
//
// The motivation is the workspace-level "always approve `run_command`" trust
// decision (S4.V Task 4): trusting the agent to run shell commands *inside*
// a folder is not the same as trusting it to run them against
// `C:\Users\me\Documents`. When `scan_outside_workspace_paths` returns a
// non-empty list, `ToolDispatcher` upgrades the effective approval policy
// from `auto_approve` back to `ask` so the user sees the call and the
// flagged tokens before it runs.
//
// Heuristics, in detection order. Each rule independently produces a
// flagged token; deduplication preserves first-seen order.
//
//   1. Absolute Windows path: `^[A-Za-z]:[/\\]...`
//   2. UNC path: `\\<server>\<share>...`
//   3. Absolute Unix-style path: `^/...` (rare on Windows but valid for
//      bash/git/wsl commands)
//   4. Parent-relative path containing `..` separator segments
//   5. Env-var expansion (`%USERPROFILE%`, `$HOME`) or `~` home-shortcut
//
// For categories 1-4 we canonicalize against `workspace_root` and only
// flag when the resolved path is NOT under the root. Category 5 is flagged
// unconditionally -- we don't try to expand env vars (that would require
// reading the parent process's environment, which is a different trust
// surface and depends on the shell that ultimately runs the command).
//
// The tokenizer respects single- and double-quoted strings as one token
// each (quotes are stripped from the returned text). It does NOT attempt
// to parse shell control flow -- nested `cmd /c "cd D:\foo && rm ..."`
// is documented as out of scope: the cd target shows up as `D:\foo` and
// gets flagged, but the rm target inside the inner string does not.
std::vector<std::string> scan_outside_workspace_paths(
    std::string_view command,
    const std::filesystem::path& workspace_root);

} // namespace locus::tools

#pragma once

#include <filesystem>
#include <string>

namespace locus {

// S4.L -- per-turn auto-commit. Free function rather than a class because the
// state lives on the workspace (config) and the agent's last assistant message
// (passed in); there's nothing for an object to own.
//
// Behaviour (no-op when any precondition fails -- never throws):
//   1. `workspace_root` must contain a `.git/` entry. If not, return success-
//      but-no-op so the caller doesn't surface a failure.
//   2. If `commit_branch` is set, switch to it (creating + warning if it
//      doesn't exist yet). The current branch position is preserved by the
//      switch via `git switch --discard-changes <branch>` if needed.
//      Note: if `commit_branch` is empty, commit goes to the current branch.
//   3. `git add -A` then `git commit -m "<prefix><truncated_message>"`.
//   4. On non-zero exit at any step, return `success=false` with stderr in
//      `error`. The caller logs a single warning -- no retry, no block.
//   5. On success, parse the just-created commit's short SHA via
//      `git rev-parse --short HEAD` and return it in `short_sha`.
//
// The truncation cap on the commit message body is ~200 chars per the stage
// spec. The prefix is NOT counted against the cap (a long prefix is the
// user's choice).
struct AutoCommitResult {
    bool        success    = false;
    bool        skipped    = false;   // not a git repo, or no message body
    std::string short_sha;            // populated on success
    std::string branch;               // branch the commit landed on
    std::string error;                // populated on failure
};

AutoCommitResult auto_commit_after_turn(const std::filesystem::path& workspace_root,
                                        const std::string&           commit_prefix,
                                        const std::string&           commit_branch,
                                        const std::string&           assistant_message);

// Truncate `text` to at most `max_chars` characters. Trailing whitespace gets
// trimmed; if truncation happens, an ellipsis "..." marker is appended within
// the cap. Multi-line input is collapsed to its first non-empty line --
// commit message subjects shouldn't span lines.
//
// Exposed for testing; callers should normally just use auto_commit_after_turn.
std::string make_commit_subject(const std::string& text, std::size_t max_chars = 200);

} // namespace locus

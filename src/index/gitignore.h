#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace locus {

namespace fs = std::filesystem;

// Minimal .gitignore support (S4.L). Reuses `glob_match()` for the actual
// pattern match -- this layer just normalises gitignore syntax into globs and
// scopes each pattern to the directory the .gitignore lives in.
//
// What we support:
//   - `#` line comments and blank lines (skipped).
//   - Trailing `/` -> directory-only pattern.
//   - Leading `/` -> anchor to the .gitignore's directory.
//   - Otherwise, pattern matches at any depth below the .gitignore's directory.
//   - `*`, `**`, `?` wildcards (delegated to glob_match).
//
// What we deliberately skip in v1 (documented in the stage spec):
//   - Negation patterns (`!`) -- treated as no-op so they can't accidentally
//     re-include files. Rare enough that adding it later behind a second pass
//     is cheap.
//   - Character classes (`[abc]`) -- glob_match doesn't support them; the
//     handful of repos that use them in .gitignore won't get those exclusions.
struct GitignorePattern {
    std::string pattern;          // forward-slashed glob pattern relative to root
    bool        directory_only;   // true if original entry ended with '/'
};

// Parse one .gitignore file's text into normalised glob patterns. The returned
// patterns are scoped to `gitignore_dir_relative` -- e.g. an entry `node_modules/`
// in `frontend/.gitignore` becomes the pattern `frontend/node_modules/**` (and a
// matching `frontend/node_modules` for the dir itself).
//
// `gitignore_dir_relative` is the workspace-relative directory the .gitignore
// lives in (empty string for the workspace-root .gitignore). It's normalised
// to use forward slashes by the caller.
std::vector<GitignorePattern>
parse_gitignore_text(const std::string& text,
                     const std::string& gitignore_dir_relative);

// Walk `root` looking for every `.gitignore` file (subject to the supplied
// per-directory skip list -- typically `{".git", ".locus", "node_modules"}`),
// parse each, and return the merged flat pattern list. Used by the indexer at
// workspace open and on watcher-driven re-reads.
//
// The skip list is a minimal performance guard: we never need to descend into
// the directories named by the explicit `exclude_patterns` to find a
// .gitignore, and walking node_modules looking for one is the most common
// real-world cost. The list isn't a substitute for the actual glob check;
// it's just an upfront prune.
std::vector<GitignorePattern>
load_workspace_gitignore(const fs::path&                  root,
                         const std::vector<std::string>&  skip_dirs = {});

// True if `rel_path` (workspace-relative, forward-slashed) matches any of
// `patterns`. `is_directory` lets directory-only patterns be respected --
// pass true when calling on a directory you're about to descend into.
bool gitignore_match(const std::vector<GitignorePattern>& patterns,
                     const std::string&                   rel_path,
                     bool                                 is_directory);

} // namespace locus

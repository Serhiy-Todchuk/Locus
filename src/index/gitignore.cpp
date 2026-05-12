#include "gitignore.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace locus {

// -- Text-line normalisation --------------------------------------------------

namespace {

// Trim CR/LF and trailing spaces. Leading spaces are significant in the
// .gitignore syntax (a pattern starting with a space matches a path starting
// with a space) -- but git treats *unescaped* trailing spaces as not
// significant, so we strip those.
std::string strip_trailing_ws(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'
                        || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

void slashify(std::string& s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
}

// Strip leading "./" -- gitignore allows it but glob_match doesn't need it.
std::string strip_leading_dot_slash(std::string s)
{
    if (s.size() >= 2 && s[0] == '.' && s[1] == '/')
        s.erase(0, 2);
    return s;
}

// Compose a workspace-relative pattern: `<dir>/<entry>`. `dir` may be empty
// (root-level .gitignore). `entry` is the post-normalisation gitignore line.
std::string scope_to_dir(const std::string& dir, const std::string& entry)
{
    if (dir.empty()) return entry;
    return dir + "/" + entry;
}

} // namespace

// -- Per-line parser ----------------------------------------------------------

std::vector<GitignorePattern>
parse_gitignore_text(const std::string& text,
                     const std::string& gitignore_dir_relative)
{
    std::vector<GitignorePattern> out;

    std::string dir = gitignore_dir_relative;
    slashify(dir);
    if (!dir.empty() && dir.back() == '/')
        dir.pop_back();

    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        line = strip_trailing_ws(std::move(line));
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        // V1 deliberately skips negation -- documented in the header. Just
        // drop the line so it can't accidentally re-include something.
        if (line[0] == '!') {
            spdlog::trace("gitignore: skipping negation '{}' (not supported in v1)", line);
            continue;
        }

        // `\#` and `\!` are escapes for literal '#' / '!' as the first char.
        if (line.size() >= 2 && line[0] == '\\'
            && (line[1] == '#' || line[1] == '!')) {
            line.erase(0, 1);
        }

        slashify(line);
        line = strip_leading_dot_slash(std::move(line));

        bool directory_only = false;
        if (!line.empty() && line.back() == '/') {
            directory_only = true;
            line.pop_back();
        }

        // Empty after the trailing slash strip? (e.g. line was just "/")
        if (line.empty()) continue;

        // Anchor handling: a leading '/' anchors the pattern to the
        // .gitignore's own directory. Strip the slash and treat the rest as
        // anchored (no implicit `**/` prefix). Otherwise, the pattern matches
        // at any depth below the directory -- prepend `**/`.
        bool anchored = false;
        if (line[0] == '/') {
            anchored = true;
            line.erase(0, 1);
            if (line.empty()) continue;
        } else if (line.find('/') != std::string::npos) {
            // A pattern containing an interior slash (e.g. "src/foo") is also
            // implicitly anchored in git's spec -- it doesn't match any
            // depth, just the directory's own subtree.
            anchored = true;
        }

        std::string base = anchored ? line : ("**/" + line);
        std::string scoped = scope_to_dir(dir, base);

        // For directory-only entries: the directory itself matches `scoped`,
        // and everything below matches `scoped/**`. Emit both so a file path
        // under the dir is caught even though directory_only means "don't
        // match files of the same name".
        if (directory_only) {
            out.push_back({scoped, /*directory_only=*/true});
            out.push_back({scoped + "/**", /*directory_only=*/false});
        } else {
            // Files OR directories. Emit both the bare pattern (matches the
            // file or dir itself) and a `/**` variant (matches everything
            // under it if it turns out to be a directory). git's semantics
            // are "the named entry plus its descendants".
            out.push_back({scoped, /*directory_only=*/false});
            out.push_back({scoped + "/**", /*directory_only=*/false});
        }
    }

    return out;
}

// -- Workspace walker ---------------------------------------------------------

std::vector<GitignorePattern>
load_workspace_gitignore(const fs::path&                  root,
                         const std::vector<std::string>&  skip_dirs)
{
    std::vector<GitignorePattern> out;

    std::unordered_set<std::string> skip_set(skip_dirs.begin(), skip_dirs.end());

    auto read_one = [&](const fs::path& gi_path, const std::string& dir_rel) {
        std::ifstream f(gi_path, std::ios::binary);
        if (!f.is_open()) return;
        std::stringstream buf;
        buf << f.rdbuf();
        auto pats = parse_gitignore_text(buf.str(), dir_rel);
        spdlog::trace("gitignore: parsed {} patterns from {}",
                      pats.size(), gi_path.string());
        out.insert(out.end(), pats.begin(), pats.end());
    };

    // Root .gitignore first so its patterns appear before nested ones in `out`
    // -- order doesn't change correctness (any-match wins) but it does make
    // log output more predictable.
    {
        auto root_gi = root / ".gitignore";
        std::error_code ec;
        if (fs::is_regular_file(root_gi, ec)) {
            read_one(root_gi, "");
        }
    }

    std::error_code walk_ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, walk_ec);
         it != fs::recursive_directory_iterator(); )
    {
        const auto& entry = *it;
        std::error_code ec;

        if (entry.is_directory(ec)) {
            const auto name = entry.path().filename().string();
            if (skip_set.count(name)) {
                it.disable_recursion_pending();
                ++it;
                continue;
            }
        }

        if (entry.is_regular_file(ec)
            && entry.path().filename() == ".gitignore")
        {
            // Skip the root one -- already handled above.
            auto rel = fs::relative(entry.path().parent_path(), root, ec);
            if (ec) { ++it; continue; }
            std::string rel_str = rel.generic_string();
            if (rel_str == ".") { ++it; continue; }
            read_one(entry.path(), rel_str);
        }

        it.increment(walk_ec);
        if (walk_ec) break;
    }

    spdlog::info("gitignore: loaded {} pattern(s) from workspace .gitignore files",
                 out.size());
    return out;
}

// -- Match -------------------------------------------------------------------

namespace {

// Recursive matcher tailored for gitignore patterns. Handles `**` properly:
// when the rest of the pattern fails after a `**` swallows N chars, it
// extends the `**` to swallow more. The shared `glob_match` (used by the
// indexer's explicit excludes) was simple two-pointer with one star slot --
// good enough for fixed patterns like `node_modules/**` but it can't recover
// when an inner `*` runs out of slash-free input.
//
// `*`  matches any non-slash characters.
// `**` matches any characters (including slashes); when followed by `/`, the
//      `/` is also consumed.
// `?`  matches one non-slash character.
bool gi_match(const char* p, std::size_t pl,
              const char* s, std::size_t sl)
{
    std::size_t pi = 0, si = 0;
    while (pi < pl) {
        if (p[pi] == '*') {
            // Lookahead for `**`.
            if (pi + 1 < pl && p[pi + 1] == '*') {
                std::size_t after = pi + 2;
                bool also_consume_slash = (after < pl && p[after] == '/');
                if (also_consume_slash) ++after;
                // Try matching the rest at every possible position from si
                // to sl (inclusive). `**` can match zero chars.
                for (std::size_t k = si; k <= sl; ++k) {
                    if (gi_match(p + after, pl - after, s + k, sl - k))
                        return true;
                }
                // If `**/` was consumed but the input had nothing left to
                // pair with the slash, we still let the recursion above
                // handle the zero-char case for the `**` followed by an
                // empty rest -- so failing here is correct.
                return false;
            }
            // Single `*` -- match any non-slash chars.
            std::size_t after = pi + 1;
            for (std::size_t k = si; k <= sl; ++k) {
                if (gi_match(p + after, pl - after, s + k, sl - k))
                    return true;
                if (k < sl && s[k] == '/') break;  // can't cross slash
            }
            return false;
        }
        if (si >= sl) return false;
        if (p[pi] == '?') {
            if (s[si] == '/') return false;
            ++pi; ++si;
            continue;
        }
        if (p[pi] != s[si]) return false;
        ++pi; ++si;
    }
    return si == sl;
}

} // namespace

bool gitignore_match(const std::vector<GitignorePattern>& patterns,
                     const std::string&                   rel_path,
                     bool                                 is_directory)
{
    if (patterns.empty()) return false;

    std::string p = rel_path;
    slashify(p);

    for (const auto& pat : patterns) {
        if (pat.directory_only && !is_directory) continue;
        if (gi_match(pat.pattern.data(), pat.pattern.size(),
                     p.data(), p.size()))
            return true;
    }
    return false;
}

} // namespace locus

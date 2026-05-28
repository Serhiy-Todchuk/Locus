#pragma once

#include <algorithm>
#include <string>

namespace locus {

namespace detail {

// Recursive glob matcher with proper `**` backtracking. `*` matches any
// sequence of non-`/` chars. `**` matches any sequence (including `/`).
// `?` matches one non-`/` char. Other chars match literally.
//
// The recursion is bounded by the pattern's `*` count (each * spawns at most
// one nested call) and the path length, so overall worst case is O(|p|*|s|)
// for typical patterns. Two-pointer impls can't backtrack through `**` when
// a later `*` consumes the slash the outer `**` should have absorbed -- the
// recursive form is the simplest correct one.
inline bool glob_match_rec(const std::string& p, size_t pi,
                           const std::string& s, size_t si)
{
    while (pi < p.size()) {
        char pc = p[pi];
        if (pc == '*') {
            if (pi + 1 < p.size() && p[pi + 1] == '*') {
                size_t np = pi + 2;
                if (np < p.size() && p[np] == '/') ++np;
                // ** absorbs zero or more chars (including '/'). Try each
                // candidate split position; matches an outer `/foo/**/bar`
                // and a degenerate `**` alike.
                for (size_t ns = si; ns <= s.size(); ++ns) {
                    if (glob_match_rec(p, np, s, ns)) return true;
                }
                return false;
            }
            size_t np = pi + 1;
            // Single * absorbs zero or more non-'/' chars. The loop body
            // stops at the first '/' so single-* never crosses a separator.
            for (size_t ns = si; ns <= s.size(); ++ns) {
                if (glob_match_rec(p, np, s, ns)) return true;
                if (ns < s.size() && s[ns] == '/') break;
            }
            return false;
        }
        if (pc == '?') {
            if (si >= s.size() || s[si] == '/') return false;
            ++pi; ++si;
            continue;
        }
        if (si >= s.size() || s[si] != pc) return false;
        ++pi; ++si;
    }
    return si == s.size();
}

}  // namespace detail

// Minimal glob match for include/exclude patterns. Handles *, ** and ?
// wildcards. Both pattern and path are normalised to forward slashes.
inline bool glob_match(const std::string& pattern, const std::string& path)
{
    std::string p = pattern;
    std::string s = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    std::replace(s.begin(), s.end(), '\\', '/');
    return detail::glob_match_rec(p, 0, s, 0);
}

} // namespace locus

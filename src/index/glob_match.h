#pragma once

#include <algorithm>
#include <string>

namespace locus {

// Minimal glob match for exclude patterns. Handles *, ** and ? wildcards.
// Both pattern and path are normalised to forward slashes internally.
inline bool glob_match(const std::string& pattern, const std::string& path)
{
    std::string p = pattern;
    std::string s = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    std::replace(s.begin(), s.end(), '\\', '/');

    size_t pi = 0, si = 0;
    size_t star_p = std::string::npos, star_s = 0;

    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '*') {
            if (pi + 1 < p.size() && p[pi + 1] == '*') {
                star_p = pi;
                pi += 2;
                if (pi < p.size() && p[pi] == '/') ++pi;
                star_s = si;
            } else {
                star_p = pi;
                ++pi;
                star_s = si;
            }
        } else if (pi < p.size() && (p[pi] == s[si] || p[pi] == '?')) {
            ++pi;
            ++si;
        } else if (star_p != std::string::npos) {
            pi = star_p;
            if (pi + 1 < p.size() && p[pi + 1] == '*') {
                ++star_s;
                si = star_s;
                pi += 2;
                if (pi < p.size() && p[pi] == '/') ++pi;
            } else {
                ++star_s;
                if (star_s <= s.size() && s[star_s - 1] == '/') {
                    return false;
                }
                si = star_s;
                pi = star_p + 1;
            }
        } else {
            return false;
        }
    }

    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

} // namespace locus

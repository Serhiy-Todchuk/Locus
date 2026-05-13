#include "mention_parser.h"

#include <cctype>

namespace locus {

namespace {

bool is_path_char(char c)
{
    // Workspace-relative paths only: alphanumeric, separators, common
    // filename punctuation. Excludes whitespace and quotes deliberately so
    // a quoted block like `"@foo bar"` stops at the space inside.
    unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) return true;
    switch (c) {
        case '/': case '\\':
        case '.': case '_': case '-':
        case '+': case '~': case '#':
            return true;
        default:
            return false;
    }
}

bool allowed_before_at(char c)
{
    // Anything that isn't an alphanumeric or `.`/`_` so we don't grab the
    // `@` in `user@example.com` or `user.name@host`. Whitespace and a few
    // punctuators are fine.
    unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) return false;
    switch (c) {
        case '.': case '_': case '-':
            return false;
        default:
            return true;
    }
}

} // namespace

std::vector<Mention> parse_mentions(std::string_view text)
{
    std::vector<Mention> out;
    const size_t n = text.size();
    for (size_t i = 0; i < n; ++i) {
        if (text[i] != '@') continue;
        if (i > 0 && !allowed_before_at(text[i - 1])) continue;

        size_t path_start = i + 1;
        size_t j = path_start;
        while (j < n && is_path_char(text[j])) ++j;

        if (j == path_start) continue;  // bare '@', no path

        // Trim trailing dots so "see @src/foo.cpp." doesn't keep the period.
        size_t end = j;
        while (end > path_start && text[end - 1] == '.') --end;
        if (end == path_start) continue;

        Mention m;
        m.start = i;
        m.end   = end;
        m.path.assign(text.data() + path_start, end - path_start);
        out.push_back(std::move(m));

        i = end - 1;  // -1 because the loop will ++i
    }
    return out;
}

} // namespace locus

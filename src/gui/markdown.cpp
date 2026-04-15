#include "markdown.h"

#include <md4c-html.h>

namespace locus {

static void append_output(const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(text, size);
}

std::string markdown_to_html(const std::string& markdown)
{
    std::string html;
    html.reserve(markdown.size() * 2);

    unsigned parser_flags = MD_DIALECT_GITHUB;
    unsigned render_flags = 0;

    int rc = md_html(markdown.c_str(),
                     static_cast<MD_SIZE>(markdown.size()),
                     append_output, &html,
                     parser_flags, render_flags);

    if (rc != 0)
        return {};

    return html;
}

} // namespace locus

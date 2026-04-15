#pragma once

#include <string>

namespace locus {

// Convert markdown text to HTML body fragment using md4c.
// Uses GitHub-flavored dialect (tables, strikethrough, task lists, autolinks).
// Returns empty string on parse failure.
std::string markdown_to_html(const std::string& markdown);

} // namespace locus

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace locus::tools {

// S6.10 Task G -- anti-truncation detection for code writes.
//
// Small local models routinely stop mid-file and emit a placeholder elision
// marker ("// rest of the code", "// ... existing code ...", etc.) before
// ending the message. WriteFileTool / EditFileTool would otherwise commit the
// truncated body to disk; the user only notices hours later when the file is
// broken.
//
// Strategy: high-precision substring scan over the last ~200 bytes of the
// proposed content. The phrase list is deliberately small -- only elision
// markers no legitimate code contains. Bare `...` / Tree-sitter ERROR-node
// detection are intentionally excluded (collides with Python Ellipsis, JS
// spread, C++ varargs, and WIP code the model is intentionally drafting).
//
// Pure CPU, no filesystem / Tree-sitter / parser dependencies.

// Returns the matched phrase (lower-cased canonical form from the phrase list)
// when truncation is detected; std::nullopt when the content looks clean.
//
// `content` is the entire proposed body; the detector slices the last 200
// chars internally so callers don't have to.
std::optional<std::string> detect_truncation_phrase(std::string_view content);

} // namespace locus::tools

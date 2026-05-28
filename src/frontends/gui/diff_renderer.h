#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace locus {

// S5.C -- inline diff rendering for chat tool results.
//
// Pure logic: takes tool args / file content, produces an HTML fragment with
// `<div class="tool-diff">` containing `<div class="diff-line {add|del|ctx}">`
// rows. The fragment is injected into the chat WebView directly (NOT through
// md4c) -- see `inject_tool_diff_html()` on ChatPanel.
//
// Lives in src/frontends/gui/ but compiles into `locus_core` (no wx deps)
// so unit tests can link against it without dragging the WebView in --
// mirrors the `ansi_parser` pattern.

// Cap on diff body emitted per tool call. Counted in *visible diff lines*
// (one `<div class="diff-line">` each, of any kind). When the cap is hit,
// emission stops and a `(N more lines collapsed)` footer is appended.
// Negative or zero is treated as "no cap" (defensive; production passes the
// WorkspaceConfig::Chat::diff_max_lines value).
struct DiffRenderOptions {
    int  max_lines     = 200;
    // Lines of unchanged context shown before and after each change. 0 means
    // "del + add only" (the pre-S5.Z behaviour). Default 4 matches `diff -u`.
    int  context_lines = 4;
    // Soft-collapse threshold (write_file only today). When the diff exceeds
    // this many rendered rows, the first N stay inline and the remainder go
    // inside a `<details>`/`<summary>` block. 0 disables collapsing.
    int  collapse_threshold = 16;
    bool dark_mode     = false;  // reserved -- v1 styling is theme-agnostic via CSS
};

// Render an `edit_file` call. Parses `args["edits_array"]` (with `args["edits"]`
// accepted as a legacy alias) -- an array of
// `{old_string, new_string, replace_all?}`) and emits one hunk per edit.
// Each hunk's `old_string` lines render as `del` and `new_string` lines as
// `add`. When `old_content` is supplied (typically the S4.B pre-mutation
// snapshot), the renderer locates each edit in the file and surrounds the
// del/add lines with `context_lines` of unchanged ctx; line numbers come
// from this content too. When `old_content` is nullopt the renderer
// degrades to del/add-only (no line numbers, no context) -- matches the
// pre-S5.Z behaviour.
//
// `path` is taken from `args["path"]` and embedded in the file header. When
// `path` is missing the header degrades to `(unknown path)`.
//
// Returns an HTML fragment (no surrounding container -- caller wraps).
// Returns the empty string when `args` is malformed / has no edits.
std::string render_edit_file_diff_html(
    const nlohmann::json&             args,
    const std::optional<std::string>& old_content,
    const DiffRenderOptions&          opts);

// Render a `write_file` call. Compares `old_content` (pre-mutation, from the
// S4.B checkpoint snapshot) against `new_content` (the bytes the tool just
// wrote) line by line using dtl's SES. A missing pre-content (nullopt) means
// the file didn't exist before -- renders as all-add.
std::string render_write_file_diff_html(
    const std::string&               path,
    const std::optional<std::string>& old_content,
    const std::string&               new_content,
    const DiffRenderOptions&         opts);

// Render a `delete_file` call. One-line summary; no diff body.
// `prev_line_count` is the number of lines in the pre-deletion content;
// pass 0 when unknown (header degrades to "deleted: `path`").
std::string render_delete_file_summary_html(
    const std::string& path,
    int                prev_line_count);

// Helper: HTML-escapes a string for safe embedding in `textContent`-free
// contexts. Exposed for tests; production code uses it via the renderers
// above.
std::string html_escape_for_diff(const std::string& s);

} // namespace locus

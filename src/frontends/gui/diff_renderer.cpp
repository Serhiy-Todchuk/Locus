#include "diff_renderer.h"

#include <dtl/dtl.hpp>

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace locus {

namespace {

// Width of each line-number column (right-aligned, space-padded). 4 fits
// up to 9999-line files comfortably; longer files just stretch the column,
// which is fine -- the line wraps inside the diff div anyway.
constexpr int k_ln_col_width = 4;

// Split `s` into lines preserving every \n boundary but stripping the
// trailing newline character itself (so each entry is the line's *visible*
// content). A trailing newline at end of input does NOT produce an empty
// final entry -- mirrors the convention used by `diff -u` (no spurious
// "no newline at end of file" hunk for routine writes). Inputs that don't
// end in \n behave identically.
std::vector<std::string> split_lines(const std::string& s)
{
    std::vector<std::string> out;
    if (s.empty()) return out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < s.size()) {
        out.emplace_back(s.substr(start));
    }
    return out;
}

// Right-align `n` in `width` spaces. n <= 0 emits a fully blank column so
// add lines have no `old` number and del lines have no `new` number.
std::string format_lineno(int n, int width = k_ln_col_width)
{
    std::string out(static_cast<std::size_t>(width), ' ');
    if (n <= 0) return out;
    std::string s = std::to_string(n);
    if (static_cast<int>(s.size()) >= width) return s;  // overflow column
    std::copy(s.rbegin(), s.rend(), out.rbegin());
    return out;
}

// Emit one diff row with two line-number columns. Either lineno can be 0
// for "blank column" (add lines get blank old, del lines get blank new).
// The textual line-number string flows through html_escape_for_diff so
// no special-case escaping is needed for the spans.
void append_diff_row(std::ostringstream& html,
                     const char*          klass,
                     int                  old_lineno,
                     int                  new_lineno,
                     const std::string&   text)
{
    html << "<div class=\"diff-line " << klass << "\">"
         << "<span class=\"diff-ln diff-ln-old\">"
         << html_escape_for_diff(format_lineno(old_lineno))
         << "</span>"
         << "<span class=\"diff-ln diff-ln-new\">"
         << html_escape_for_diff(format_lineno(new_lineno))
         << "</span>"
         << "<span class=\"diff-text\">"
         << html_escape_for_diff(text)
         << "</span>"
         << "</div>";
}

void append_truncation_footer(std::ostringstream& html, int hidden_lines)
{
    html << "<div class=\"diff-truncated\">("
         << hidden_lines
         << " more line" << (hidden_lines == 1 ? "" : "s")
         << " not shown)</div>";
}

void append_file_header(std::ostringstream& html,
                        const char*         tool_name,
                        const std::string&  path)
{
    html << "<div class=\"diff-file-header\">"
         << tool_name << ": <code>"
         << html_escape_for_diff(path.empty() ? "(unknown path)" : path)
         << "</code></div>";
}

// Locate the first occurrence of `needle` in `haystack` and return the
// 1-based starting line number, or 0 if not found. Lines are delimited
// by \n; the final line without a trailing \n still counts.
int find_line_of_match(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return 0;
    auto pos = haystack.find(needle);
    if (pos == std::string::npos) return 0;
    int line = 1;
    for (std::size_t i = 0; i < pos; ++i) {
        if (haystack[i] == '\n') ++line;
    }
    return line;
}

// In-place replace of the first occurrence of `from` with `to` in `s`.
// Returns true if replaced. Mirrors edit_file's atomic single-edit when
// replace_all is false.
bool replace_first(std::string& s, const std::string& from, const std::string& to)
{
    auto pos = s.find(from);
    if (pos == std::string::npos) return false;
    s.replace(pos, from.size(), to);
    return true;
}

// Replace every non-overlapping occurrence. Counts replacements via the
// returned value. Used for replace_all edits in the same way file_tools.cpp
// applies them.
int replace_all_count(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return 0;
    int count = 0;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
        ++count;
    }
    return count;
}

} // namespace

std::string html_escape_for_diff(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            // Strip carriage returns so CRLF input doesn't paint a stray box
            // char or break the line layout. \r alone (rare) is dropped too.
            case '\r': break;
            default:   out += c;        break;
        }
    }
    return out;
}

// -- edit_file ---------------------------------------------------------------

std::string render_edit_file_diff_html(const nlohmann::json&             args,
                                       const std::optional<std::string>& old_content,
                                       const DiffRenderOptions&          opts)
{
    // Canonical key is `edits_array` (uniform compound-name signature);
    // legacy `edits` accepted so saved sessions still render.
    if (!args.is_object()) return {};
    const char* edits_key = nullptr;
    if (args.contains("edits_array") && args["edits_array"].is_array())
        edits_key = "edits_array";
    else if (args.contains("edits") && args["edits"].is_array())
        edits_key = "edits";
    if (!edits_key) return {};

    const auto& edits = args[edits_key];
    if (edits.empty()) return {};

    // Canonical arg is `file_path` (Claude Code convention); `path` accepted
    // as a legacy alias so saved sessions + unit tests continue to render.
    std::string path = args.value("file_path", std::string{});
    if (path.empty()) path = args.value("path", std::string{});

    std::ostringstream html;
    html << "<div class=\"tool-diff\">";
    append_file_header(html, "edit_file", path);

    int  emitted_lines = 0;
    int  hidden_lines  = 0;
    bool truncated     = false;
    const int max         = opts.max_lines > 0     ? opts.max_lines     : 0;
    const int ctx_n       = opts.context_lines > 0 ? opts.context_lines : 0;

    auto cap_check_and_inc = [&]() -> bool {
        if (max > 0 && emitted_lines >= max) {
            ++hidden_lines;
            truncated = true;
            return false;
        }
        ++emitted_lines;
        return true;
    };

    auto emit_row = [&](const char* klass, int oldln, int newln,
                        const std::string& text) {
        if (!cap_check_and_inc()) return;
        append_diff_row(html, klass, oldln, newln, text);
    };

    // Working buffers for sequential edit application. `working_old` tracks
    // the state of the file as previous edits would have left it (so this
    // edit's `old_string` is matched in the same place edit_file did).
    // `cumulative_old_offset` / `cumulative_new_offset` accumulate line-count
    // deltas so context line numbers stay consistent across multiple edits
    // in the same call.
    std::string working_old;
    bool        have_old_content = old_content.has_value();
    if (have_old_content) working_old = *old_content;

    int edit_index = 0;
    const int total_edits = static_cast<int>(edits.size());
    for (const auto& edit : edits) {
        ++edit_index;
        const std::string old_s = edit.value("old_string", std::string{});
        const std::string new_s = edit.value("new_string", std::string{});
        const bool replace_all  = edit.value("replace_all", false);

        // Locate this edit in `working_old`. 0 = not found (rare -- could
        // happen when a batched edit depends on a prior edit's output that
        // didn't apply, or when no old_content was supplied). The renderer
        // degrades to "del + add, no line numbers, no context" in that case.
        int match_line = have_old_content
            ? find_line_of_match(working_old, old_s)
            : 0;

        // Per-edit hunk header: the textual `@@ edit N/M @@` already in v1,
        // now augmented with the file line range when known. Counted against
        // the cap.
        std::ostringstream hdr;
        hdr << "@@ edit " << edit_index << "/" << total_edits;
        if (replace_all) hdr << " (replace_all)";
        if (match_line > 0) hdr << " @ line " << match_line;
        hdr << " @@";
        if (cap_check_and_inc()) {
            html << "<div class=\"diff-hunk-header\">"
                 << html_escape_for_diff(hdr.str())
                 << "</div>";
        }

        auto old_lines = split_lines(old_s);
        auto new_lines = split_lines(new_s);

        if (match_line <= 0) {
            // No context available -- straight del-then-add (the pre-S5.Z
            // path). Line numbers blank.
            for (const auto& l : old_lines) emit_row("del", 0, 0, l);
            for (const auto& l : new_lines) emit_row("add", 0, 0, l);
            continue;
        }

        auto working_lines = split_lines(working_old);
        const int total_lines = static_cast<int>(working_lines.size());
        const int del_count   = static_cast<int>(old_lines.size());
        const int add_count   = static_cast<int>(new_lines.size());

        // Context-before: up to ctx_n lines preceding the match.
        int ctx_before_start = std::max(1, match_line - ctx_n);
        for (int ln = ctx_before_start; ln < match_line; ++ln) {
            emit_row("ctx", ln, ln, working_lines[static_cast<std::size_t>(ln - 1)]);
        }

        // Del lines -- numbered with their old-file line.
        for (int i = 0; i < del_count; ++i) {
            emit_row("del", match_line + i, 0, old_lines[static_cast<std::size_t>(i)]);
        }

        // Add lines -- numbered with their new-file line. Approximation:
        // assume the file is renumbered locally so add lines occupy
        // [match_line, match_line + add_count - 1] in the new file. For
        // the FIRST edit this matches the actual layout; for batched
        // edits the column is best-effort (still useful for orientation).
        for (int i = 0; i < add_count; ++i) {
            emit_row("add", 0, match_line + i, new_lines[static_cast<std::size_t>(i)]);
        }

        // Context-after: up to ctx_n lines following the matched span.
        int ctx_after_start = match_line + del_count;
        int ctx_after_end   = std::min(total_lines, ctx_after_start + ctx_n - 1);
        for (int ln = ctx_after_start; ln <= ctx_after_end; ++ln) {
            // New-line column estimate: shift by (add_count - del_count)
            // accumulated so far in this hunk.
            int new_ln = ln + (add_count - del_count);
            emit_row("ctx", ln, new_ln,
                     working_lines[static_cast<std::size_t>(ln - 1)]);
        }

        // For replace_all, surface the additional-occurrence count so the
        // user knows the rendered hunk is just the first of N.
        if (replace_all) {
            std::string probe = working_old;
            int total_replacements = replace_all_count(probe, old_s, new_s);
            if (total_replacements > 1) {
                std::ostringstream note;
                note << "(+" << (total_replacements - 1)
                     << " more occurrence"
                     << (total_replacements - 1 == 1 ? "" : "s")
                     << " replaced -- not shown)";
                if (cap_check_and_inc()) {
                    html << "<div class=\"diff-truncated\">"
                         << html_escape_for_diff(note.str())
                         << "</div>";
                }
            }
        }

        // Apply this edit to `working_old` so the next iteration sees the
        // post-edit state -- matches the atomicity story in file_tools.cpp.
        if (replace_all) {
            replace_all_count(working_old, old_s, new_s);
        } else {
            replace_first(working_old, old_s, new_s);
        }
    }

    if (truncated) append_truncation_footer(html, hidden_lines);

    html << "</div>";
    return html.str();
}

// -- write_file --------------------------------------------------------------

// Compress an SES into hunks: keep only `context_lines` of unchanged ctx
// before/after each change run, and emit a `<div class="diff-truncated">`
// when an internal ctx gap of N is collapsed.
//
// `entries` is the dtl SES sequence: (text, ses_info). `ctx_n` is the
// surrounding-context budget.
template <typename Sink>
void emit_compressed_ses(
    const std::vector<std::pair<std::string, dtl::elemInfo>>& entries,
    int   ctx_n,
    Sink&& sink)
{
    // First pass: classify each row into kind + (old_lineno, new_lineno).
    struct Row {
        char        kind;   // ' ' = ctx, '+' = add, '-' = del
        int         old_ln; // 0 = blank
        int         new_ln; // 0 = blank
        std::string text;
    };
    std::vector<Row> rows;
    rows.reserve(entries.size());
    int old_ln = 0, new_ln = 0;
    for (const auto& [text, info] : entries) {
        Row r;
        r.text = text;
        if (info.type == dtl::SES_COMMON) {
            ++old_ln; ++new_ln;
            r.kind = ' '; r.old_ln = old_ln; r.new_ln = new_ln;
        } else if (info.type == dtl::SES_ADD) {
            ++new_ln;
            r.kind = '+'; r.old_ln = 0;      r.new_ln = new_ln;
        } else {
            ++old_ln;
            r.kind = '-'; r.old_ln = old_ln; r.new_ln = 0;
        }
        rows.push_back(std::move(r));
    }

    // Mark rows that should survive the trim: any non-ctx row, plus up to
    // ctx_n ctx rows before/after each change.
    std::vector<bool> keep(rows.size(), false);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == ' ') continue;
        std::size_t lo = (i >= static_cast<std::size_t>(ctx_n))
                           ? i - static_cast<std::size_t>(ctx_n) : 0;
        std::size_t hi = std::min(rows.size() - 1,
                                  i + static_cast<std::size_t>(ctx_n));
        for (std::size_t j = lo; j <= hi; ++j) keep[j] = true;
    }

    // Walk rows, emitting kept ones; when we hit a kept->not-kept transition
    // followed by a not-kept->kept transition, emit a single truncation
    // marker with the count of collapsed lines.
    std::size_t i = 0;
    while (i < rows.size()) {
        if (keep[i]) {
            sink.emit(rows[i].kind, rows[i].old_ln, rows[i].new_ln, rows[i].text);
            ++i;
            continue;
        }
        std::size_t skip_start = i;
        while (i < rows.size() && !keep[i]) ++i;
        int skipped = static_cast<int>(i - skip_start);
        if (skipped > 0) sink.skip(skipped);
    }
}

std::string render_write_file_diff_html(
    const std::string&                path,
    const std::optional<std::string>& old_content,
    const std::string&                new_content,
    const DiffRenderOptions&          opts)
{
    std::ostringstream html;
    html << "<div class=\"tool-diff\">";
    append_file_header(html, "write_file", path);

    const int max         = opts.max_lines > 0     ? opts.max_lines     : 0;
    const int ctx_n       = opts.context_lines > 0 ? opts.context_lines : 0;
    const int collapse    = opts.collapse_threshold > 0 ? opts.collapse_threshold : 0;
    int hidden_lines  = 0;
    bool truncated    = false;

    // Rows accumulate into a vector first so the assembler at the end can
    // decide whether the body crossed the soft-collapse threshold and needs
    // to be split into "inline + <details>" sections.
    std::vector<std::string> rows;
    rows.reserve(64);

    auto emit_str = [&](std::string row) {
        if (max > 0 && static_cast<int>(rows.size()) >= max) {
            ++hidden_lines;
            truncated = true;
            return;
        }
        rows.push_back(std::move(row));
    };

    auto emit_row = [&](const char* klass, int oldln, int newln,
                        const std::string& text) {
        std::ostringstream s;
        append_diff_row(s, klass, oldln, newln, text);
        emit_str(s.str());
    };

    auto new_lines = split_lines(new_content);

    if (!old_content.has_value()) {
        // New file: every line is an add, numbered in the new-file column.
        int ln = 0;
        for (const auto& l : new_lines) {
            ++ln;
            emit_row("add", 0, ln, l);
        }
    } else {
        auto old_lines = split_lines(*old_content);
        if (old_lines == new_lines) {
            std::ostringstream s;
            s << "<div class=\"diff-line ctx\">"
              << "<span class=\"diff-ln diff-ln-old\">"
              << html_escape_for_diff(format_lineno(0))
              << "</span>"
              << "<span class=\"diff-ln diff-ln-new\">"
              << html_escape_for_diff(format_lineno(0))
              << "</span>"
              << "<span class=\"diff-text\">(no changes)</span>"
              << "</div>";
            emit_str(s.str());
        } else {
            dtl::Diff<std::string> diff(old_lines, new_lines);
            diff.compose();
            const auto& seq = diff.getSes().getSequence();
            // Materialise SES into a vector so emit_compressed_ses can do
            // two passes without copying the dtl-internal container.
            std::vector<std::pair<std::string, dtl::elemInfo>> entries(
                seq.begin(), seq.end());

            struct Sink {
                std::function<void(const char*, int, int, const std::string&)> emit_fn;
                std::function<void(int)> skip_fn;
                void emit(char kind, int old_ln, int new_ln,
                          const std::string& text) {
                    const char* klass =
                        kind == '+' ? "add" :
                        kind == '-' ? "del" : "ctx";
                    emit_fn(klass, old_ln, new_ln, text);
                }
                void skip(int n) { skip_fn(n); }
            };
            Sink sink;
            sink.emit_fn = [&](const char* klass, int oldln, int newln,
                               const std::string& text) {
                emit_row(klass, oldln, newln, text);
            };
            sink.skip_fn = [&](int n) {
                std::ostringstream s;
                s << "<div class=\"diff-truncated\">"
                  << html_escape_for_diff(
                        std::string("... ") + std::to_string(n) +
                        " unchanged line" + (n == 1 ? "" : "s") + " ...")
                  << "</div>";
                emit_str(s.str());
            };
            emit_compressed_ses(entries, ctx_n, sink);
        }
    }

    // Soft-collapse assembly. Show up to `collapse` rows inline; fold any
    // remainder into a native <details>/<summary> toggle. <details>'s open
    // attribute is omitted on purpose (default is closed -- "16 lines plus
    // a one-click reveal" was the whole point of the user's request).
    const int total = static_cast<int>(rows.size());
    if (collapse > 0 && total > collapse) {
        for (int i = 0; i < collapse; ++i) html << rows[i];
        const int hidden = total - collapse;
        html << "<details class=\"diff-collapsed\"><summary>Show "
             << hidden << " more line" << (hidden == 1 ? "" : "s")
             << "</summary>";
        for (int i = collapse; i < total; ++i) html << rows[i];
        html << "</details>";
    } else {
        for (const auto& r : rows) html << r;
    }

    if (truncated) append_truncation_footer(html, hidden_lines);

    html << "</div>";
    return html.str();
}

// -- delete_file -------------------------------------------------------------

std::string render_delete_file_summary_html(const std::string& path,
                                            int                prev_line_count)
{
    std::ostringstream html;
    html << "<div class=\"tool-diff diff-deleted\">"
         << "<div class=\"diff-file-header\">delete_file: <code>"
         << html_escape_for_diff(path.empty() ? "(unknown path)" : path)
         << "</code>";
    if (prev_line_count > 0) {
        html << " <span class=\"diff-meta\">("
             << prev_line_count
             << " line" << (prev_line_count == 1 ? "" : "s")
             << ")</span>";
    }
    html << "</div></div>";
    return html.str();
}

} // namespace locus

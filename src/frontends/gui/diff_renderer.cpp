#include "diff_renderer.h"

#include <dtl/dtl.hpp>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace locus {

namespace {

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

void append_line_div(std::ostringstream& html,
                     const char*          klass,
                     const std::string&   text)
{
    html << "<div class=\"diff-line " << klass << "\">"
         << html_escape_for_diff(text)
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
                        const std::string&  tool_name,
                        const std::string&  path)
{
    html << "<div class=\"diff-file-header\">"
         << tool_name << ": <code>"
         << html_escape_for_diff(path.empty() ? "(unknown path)" : path)
         << "</code></div>";
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

std::string render_edit_file_diff_html(const nlohmann::json&    args,
                                       const DiffRenderOptions& opts)
{
    if (!args.is_object() || !args.contains("edits") || !args["edits"].is_array())
        return {};

    const auto& edits = args["edits"];
    if (edits.empty()) return {};

    const std::string path = args.value("path", std::string{});

    std::ostringstream html;
    html << "<div class=\"tool-diff\">";
    append_file_header(html, "edit_file", path);

    int  emitted_lines = 0;
    int  hidden_lines  = 0;
    bool truncated     = false;
    const int max = opts.max_lines > 0 ? opts.max_lines : 0;

    auto emit_line = [&](const char* klass, const std::string& text) {
        if (max > 0 && emitted_lines >= max) {
            ++hidden_lines;
            truncated = true;
            return;
        }
        append_line_div(html, klass, text);
        ++emitted_lines;
    };

    int edit_index = 0;
    const int total_edits = static_cast<int>(edits.size());
    for (const auto& edit : edits) {
        ++edit_index;
        const std::string old_s = edit.value("old_string", std::string{});
        const std::string new_s = edit.value("new_string", std::string{});
        const bool replace_all  = edit.value("replace_all", false);

        // Per-edit hunk header. Counted against the cap so a thousand-edit
        // batch can't paint a thousand headers past the truncation point.
        std::ostringstream hdr;
        hdr << "@@ edit " << edit_index << "/" << total_edits;
        if (replace_all) hdr << " (replace_all)";
        hdr << " @@";
        if (max > 0 && emitted_lines >= max) {
            ++hidden_lines;
            truncated = true;
        } else {
            html << "<div class=\"diff-hunk-header\">"
                 << html_escape_for_diff(hdr.str())
                 << "</div>";
            ++emitted_lines;
        }

        for (const auto& l : split_lines(old_s)) emit_line("del", l);
        for (const auto& l : split_lines(new_s)) emit_line("add", l);
    }

    if (truncated) append_truncation_footer(html, hidden_lines);

    html << "</div>";
    return html.str();
}

// -- write_file --------------------------------------------------------------

std::string render_write_file_diff_html(
    const std::string&                path,
    const std::optional<std::string>& old_content,
    const std::string&                new_content,
    const DiffRenderOptions&          opts)
{
    std::ostringstream html;
    html << "<div class=\"tool-diff\">";
    append_file_header(html, "write_file", path);

    const int max = opts.max_lines > 0 ? opts.max_lines : 0;
    int emitted_lines = 0;
    int hidden_lines  = 0;
    bool truncated    = false;

    auto emit_line = [&](const char* klass, const std::string& text) {
        if (max > 0 && emitted_lines >= max) {
            ++hidden_lines;
            truncated = true;
            return;
        }
        append_line_div(html, klass, text);
        ++emitted_lines;
    };

    auto new_lines = split_lines(new_content);

    if (!old_content.has_value()) {
        // New file: render every line as add. No header needed -- the file
        // header already says "write_file: <path>".
        for (const auto& l : new_lines) emit_line("add", l);
    } else {
        auto old_lines = split_lines(*old_content);
        if (old_lines == new_lines) {
            // No-op overwrite (same bytes). Still useful to surface; render
            // as zero-line summary -- "no changes" reads better than an
            // empty diff body.
            html << "<div class=\"diff-line ctx\">(no changes)</div>";
        } else {
            dtl::Diff<std::string> diff(old_lines, new_lines);
            diff.compose();
            const auto& ses = diff.getSes().getSequence();
            for (const auto& [text, info] : ses) {
                const char* klass = "ctx";
                if      (info.type == dtl::SES_ADD)    klass = "add";
                else if (info.type == dtl::SES_DELETE) klass = "del";
                emit_line(klass, text);
            }
        }
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

#include "tools/file_tools.h"
#include "tools/shared.h"

#include "core/workspace_services.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace locus {

namespace fs = std::filesystem;

using tools::error_result;
using tools::ReadTracker;
using tools::resolve_path;

// -- ReadFileTool -----------------------------------------------------------

std::string ReadFileTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    int offset = call.args.value("offset", 1);
    int length = call.args.value("length", 100);
    return "Read " + path + " lines " + std::to_string(offset) + "-" +
           std::to_string(offset + length - 1);
}

ToolResult ReadFileTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string rel_path = call.args.value("path", "");
    int offset = call.args.value("offset", 1);
    int length = call.args.value("length", 100);

    if (rel_path.empty())
        return error_result("Error: 'path' parameter is required");
    if (offset < 1) offset = 1;
    if (length < 1) length = 100;

    fs::path full = resolve_path(ws, rel_path);
    if (full.empty())
        return error_result("Error: path resolves outside workspace");

    std::ifstream file(full);
    if (!file.is_open())
        return error_result("Error: cannot open file: " + rel_path);

    std::string line;
    std::ostringstream content;
    int lines_read = 0;
    int total_lines = 0;

    while (std::getline(file, line)) {
        ++total_lines;
        if (total_lines >= offset && lines_read < length) {
            content << total_lines << "\t" << line << "\n";
            ++lines_read;
        }
    }

    if (lines_read == 0 && total_lines == 0)
        return {true, "(empty file)", "(empty file)"};

    if (offset > total_lines)
        return error_result("Error: offset " + std::to_string(offset) +
                            " exceeds file length (" + std::to_string(total_lines) + " lines)");

    std::string header = "[" + rel_path + " lines " + std::to_string(offset) + "-" +
                         std::to_string(std::min(offset + lines_read - 1, total_lines)) +
                         " of " + std::to_string(total_lines) + "]\n";

    std::string result_content = header + content.str();
    std::string result_display = result_content;

    spdlog::trace("read_file: {} (lines {}-{} of {})", rel_path, offset,
                  offset + lines_read - 1, total_lines);

    // S4.A: mark this path as "agent has seen it" so edit_file / multi_edit_file
    // will accept subsequent edits without complaining.
    ReadTracker::instance().mark_read(full);

    return {true, result_content, result_display};
}

// -- WriteFileTool ----------------------------------------------------------

std::string WriteFileTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    int content_len = call.args.value("content", "").size();
    return "Overwrite " + path + " (" + std::to_string(content_len) + " bytes)";
}

ToolResult WriteFileTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string rel_path = call.args.value("path", "");
    std::string content  = call.args.value("content", "");

    if (rel_path.empty())
        return error_result("Error: 'path' parameter is required");

    fs::path full = resolve_path(ws, rel_path);
    if (full.empty())
        return error_result("Error: path resolves outside workspace");

    if (!fs::exists(full))
        return error_result("Error: file does not exist: " + rel_path +
                            " (use create_file for new files)");

    std::ofstream out(full, std::ios::trunc);
    if (!out.is_open())
        return error_result("Error: cannot write to file: " + rel_path);

    out << content;
    out.close();

    spdlog::trace("write_file: {} ({} bytes)", rel_path, content.size());

    std::string msg = "Wrote " + std::to_string(content.size()) + " bytes to " + rel_path;
    return {true, msg, msg};
}

// -- CreateFileTool ---------------------------------------------------------

std::string CreateFileTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    return "Create new file: " + path;
}

ToolResult CreateFileTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string rel_path = call.args.value("path", "");
    std::string content  = call.args.value("content", "");

    if (rel_path.empty())
        return error_result("Error: 'path' parameter is required");

    fs::path full = resolve_path(ws, rel_path);
    if (full.empty())
        return error_result("Error: path resolves outside workspace");

    if (fs::exists(full))
        return error_result("Error: file already exists: " + rel_path +
                            " (use write_file to overwrite)");

    std::error_code ec;
    fs::create_directories(full.parent_path(), ec);
    if (ec)
        return error_result("Error: cannot create directories: " + ec.message());

    std::ofstream out(full);
    if (!out.is_open())
        return error_result("Error: cannot create file: " + rel_path);

    out << content;
    out.close();

    spdlog::trace("create_file: {} ({} bytes)", rel_path, content.size());

    std::string msg = "Created " + rel_path + " (" + std::to_string(content.size()) + " bytes)";
    return {true, msg, msg};
}

// -- Edit helpers -----------------------------------------------------------

namespace {

// Count non-overlapping occurrences of `needle` in `haystack`.
// Empty `needle` returns 0 (the caller treats empty old_string as an error).
size_t count_occurrences(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// Replace every non-overlapping occurrence. In-place-safe: walks left-to-right
// on a fresh string so that `new_string` containing `old_string` does not
// trigger rescans.
std::string replace_all_occurrences(const std::string& haystack,
                                    const std::string& needle,
                                    const std::string& replacement)
{
    if (needle.empty()) return haystack;
    std::string out;
    out.reserve(haystack.size());
    size_t prev = 0;
    size_t pos  = 0;
    while ((pos = haystack.find(needle, prev)) != std::string::npos) {
        out.append(haystack, prev, pos - prev);
        out.append(replacement);
        prev = pos + needle.size();
    }
    out.append(haystack, prev, std::string::npos);
    return out;
}

// Load a file's bytes as-is (binary mode to preserve \r\n line endings).
bool slurp(const fs::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Apply a single (old_string, new_string, replace_all) edit to `buf` in place.
// Returns empty string on success, otherwise a human-readable error describing
// why the edit could not be applied. The rel_path is folded into the message.
std::string apply_single_edit(std::string& buf,
                              const std::string& old_s,
                              const std::string& new_s,
                              bool replace_all,
                              const std::string& rel_path)
{
    if (old_s.empty())
        return "Error: 'old_string' is empty in " + rel_path;
    if (old_s == new_s)
        return "Error: 'old_string' equals 'new_string' in " + rel_path +
               " (no-op)";

    size_t count = count_occurrences(buf, old_s);
    if (count == 0)
        return "Error: 'old_string' not found in " + rel_path +
               " (exact match required, including whitespace)";
    if (count > 1 && !replace_all)
        return "Error: 'old_string' matches " + std::to_string(count) +
               " locations in " + rel_path +
               ". Widen the match with more surrounding context, or set "
               "replace_all=true to replace every occurrence.";

    if (replace_all || count == 1) {
        buf = replace_all_occurrences(buf, old_s, new_s);
    }
    return {};
}

// Atomic write-back: write to <path>.locus-tmp then rename over the original.
// Keeps the file intact if the process is killed mid-write.
std::string write_atomic(const fs::path& target, const std::string& content)
{
    fs::path tmp = target;
    tmp += ".locus-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return "cannot open temp file for write: " + tmp.string();
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out.good())
            return "write failed on temp file: " + tmp.string();
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        // Fallback: rename can fail cross-device / with readers holding handles.
        // Copy + remove tmp is the safety net.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) return "rename/copy to target failed: " + ec.message();
    }
    return {};
}

// Build a compact unified-diff-like preview: "- <old_snippet>\n+ <new_snippet>".
// Truncates snippets longer than ~80 chars. Used by preview() so the approval
// pane shows *what changes*, not just the path.
std::string make_edit_preview(const std::string& old_s, const std::string& new_s)
{
    auto snip = [](std::string s) {
        for (auto& c : s) if (c == '\n') c = ' ';
        if (s.size() > 80) s = s.substr(0, 77) + "...";
        return s;
    };
    return "- " + snip(old_s) + "\n+ " + snip(new_s);
}

} // namespace

// -- EditFileTool -----------------------------------------------------------

std::string EditFileTool::preview(const ToolCall& call) const
{
    std::string path  = call.args.value("path", "");
    std::string old_s = call.args.value("old_string", "");
    std::string new_s = call.args.value("new_string", "");
    bool all          = call.args.value("replace_all", false);
    std::string head  = "Edit " + path + (all ? " (replace_all)" : "") + "\n";
    return head + make_edit_preview(old_s, new_s);
}

ToolResult EditFileTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string rel_path = call.args.value("path", "");
    std::string old_s    = call.args.value("old_string", "");
    std::string new_s    = call.args.value("new_string", "");
    bool replace_all     = call.args.value("replace_all", false);

    if (rel_path.empty())
        return error_result("Error: 'path' parameter is required");

    fs::path full = resolve_path(ws, rel_path);
    if (full.empty())
        return error_result("Error: path resolves outside workspace");
    if (!fs::exists(full))
        return error_result("Error: file does not exist: " + rel_path +
                            " (use create_file for new files)");

    if (!ReadTracker::instance().was_read(full))
        return error_result("Error: read the file first. Call read_file on '" +
                            rel_path +
                            "' before edit_file so the exact content is confirmed.");

    std::string buf;
    if (!slurp(full, buf))
        return error_result("Error: cannot open file for read: " + rel_path);

    std::string err = apply_single_edit(buf, old_s, new_s, replace_all, rel_path);
    if (!err.empty()) return error_result(err);

    std::string werr = write_atomic(full, buf);
    if (!werr.empty())
        return error_result("Error: " + werr);

    spdlog::trace("edit_file: {} ({}{})", rel_path,
                  replace_all ? "replace_all, " : "",
                  std::to_string(buf.size()) + " bytes");

    std::string msg = "Edited " + rel_path;
    if (replace_all)
        msg += " (all occurrences)";
    return {true, msg, msg};
}

// -- MultiEditFileTool ------------------------------------------------------

std::string MultiEditFileTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    const auto& edits = call.args.value("edits", nlohmann::json::array());
    std::ostringstream ss;
    ss << "Edit " << path << " (" << edits.size() << " replacement"
       << (edits.size() == 1 ? "" : "s") << ")\n";
    int n = 0;
    for (const auto& e : edits) {
        ss << make_edit_preview(e.value("old_string", ""),
                                e.value("new_string", ""));
        if (++n >= 3 && edits.size() > 3) {
            ss << "\n... +" << (edits.size() - 3) << " more";
            break;
        }
        if (n < static_cast<int>(edits.size())) ss << "\n";
    }
    return ss.str();
}

ToolResult MultiEditFileTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string rel_path = call.args.value("path", "");
    if (rel_path.empty())
        return error_result("Error: 'path' parameter is required");

    if (!call.args.contains("edits") || !call.args["edits"].is_array())
        return error_result("Error: 'edits' must be an array of "
                            "{old_string, new_string, replace_all?} objects");
    const auto& edits = call.args["edits"];
    if (edits.empty())
        return error_result("Error: 'edits' array is empty");

    fs::path full = resolve_path(ws, rel_path);
    if (full.empty())
        return error_result("Error: path resolves outside workspace");
    if (!fs::exists(full))
        return error_result("Error: file does not exist: " + rel_path);

    if (!ReadTracker::instance().was_read(full))
        return error_result("Error: read the file first. Call read_file on '" +
                            rel_path +
                            "' before multi_edit_file so the exact content is confirmed.");

    std::string buf;
    if (!slurp(full, buf))
        return error_result("Error: cannot open file for read: " + rel_path);

    // Apply every edit to an in-memory buffer. Any failure → discard buffer,
    // file on disk is unchanged (atomicity).
    for (size_t i = 0; i < edits.size(); ++i) {
        const auto& e = edits[i];
        std::string old_s = e.value("old_string", "");
        std::string new_s = e.value("new_string", "");
        bool all          = e.value("replace_all", false);

        std::string err = apply_single_edit(buf, old_s, new_s, all, rel_path);
        if (!err.empty()) {
            return error_result("Error: edit " + std::to_string(i + 1) + "/" +
                                std::to_string(edits.size()) + " failed: " + err +
                                " (no changes written — multi_edit is atomic)");
        }
    }

    std::string werr = write_atomic(full, buf);
    if (!werr.empty())
        return error_result("Error: " + werr);

    spdlog::trace("multi_edit_file: {} ({} edits, {} bytes)",
                  rel_path, edits.size(), buf.size());

    std::string msg = "Applied " + std::to_string(edits.size()) +
                      " edit" + (edits.size() == 1 ? "" : "s") +
                      " to " + rel_path;
    return {true, msg, msg};
}

// -- DeleteFileTool ---------------------------------------------------------

std::string DeleteFileTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    return "WARNING: Permanently delete " + path;
}

ToolResult DeleteFileTool::execute(const ToolCall& call, IWorkspaceServices& ws)
{
    std::string rel_path = call.args.value("path", "");

    if (rel_path.empty())
        return error_result("Error: 'path' parameter is required");

    fs::path full = resolve_path(ws, rel_path);
    if (full.empty())
        return error_result("Error: path resolves outside workspace");

    if (!fs::exists(full))
        return error_result("Error: file does not exist: " + rel_path);

    if (fs::is_directory(full))
        return error_result("Error: path is a directory, not a file: " + rel_path);

    std::error_code ec;
    fs::remove(full, ec);
    if (ec)
        return error_result("Error: cannot delete file: " + ec.message());

    spdlog::trace("delete_file: {}", rel_path);

    std::string msg = "Deleted " + rel_path;
    return {true, msg, msg};
}

} // namespace locus

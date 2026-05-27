#include "tools/file_tools.h"
#include "tools/shared.h"
#include "tools/truncation_detector.h"

#include "core/workspace.h"
#include "core/workspace_services.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace locus {

namespace fs = std::filesystem;

using tools::detect_truncation_phrase;
using tools::error_result;
using tools::ReadTracker;
using tools::resolve_path;
using tools::write_atomic;

namespace {

// S6.10 Task G -- run the truncation detector against `body` if the workspace
// has the flag on. Returns an error_result-style ToolResult on detection
// (success=false, content carrying the matched phrase + override hint);
// returns nullopt when the body looks clean OR when the flag is off.
std::optional<ToolResult> check_truncation(IWorkspaceServices& ws,
                                            const std::string& body,
                                            const std::string& rel_path,
                                            const std::string& tool_name)
{
    // Respect the per-workspace knob; opt out cleanly when no workspace
    // is wired (the in-process tests use a FakeWorkspaceServices).
    Workspace* w = ws.workspace();
    if (!w || !w->config().agent.detect_write_truncation) return std::nullopt;

    auto match = detect_truncation_phrase(body);
    if (!match) return std::nullopt;

    spdlog::warn("{}: truncation marker '{}' detected near end of {} -- refusing",
                 tool_name, *match, rel_path);

    std::string msg =
        "Error: the proposed content for '" + rel_path +
        "' appears truncated. Matched the elision marker '" + *match +
        "' near the end of the body. Emit the complete file content -- "
        "placeholders like 'rest of the code' indicate the model elided real "
        "content. If this string is intentional in your file, set "
        "agent.detect_write_truncation=false in .locus/config.json.";
    ToolResult r{false, msg, msg};
    r.activity_tag     = "truncation_blocked";
    r.activity_summary = "Write blocked: truncation marker in " + rel_path;
    r.activity_detail  = "tool: " + tool_name +
                         "\npath: " + rel_path +
                         "\nphrase: " + *match;
    return r;
}

} // namespace

// -- ReadFileTool -----------------------------------------------------------

std::string ReadFileTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    int offset = call.args.value("offset", 1);
    int length = call.args.value("length", 100);
    return "Read " + path + " lines " + std::to_string(offset) + "-" +
           std::to_string(offset + length - 1);
}

ToolResult ReadFileTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                  const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call, {"path", "offset", "length"}))
        return *err;

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

    // S6.17 Task D -- embed the args actually applied so the model can
    // self-correct when a key it sent was silently honoured-as-default.
    std::string header = "[" + rel_path + " lines " + std::to_string(offset) + "-" +
                         std::to_string(std::min(offset + lines_read - 1, total_lines)) +
                         " of " + std::to_string(total_lines) +
                         " (offset=" + std::to_string(offset) +
                         " length=" + std::to_string(length) + ")]\n";

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
    // No workspace access from inside preview() -- surface the intent as the
    // LLM declared it. The execute path is what actually enforces create-vs-
    // overwrite based on the real file state.
    std::string path = call.args.value("path", "");
    size_t new_len   = call.args.value("content", "").size();
    bool overwrite   = call.args.value("overwrite", false);

    std::string head = overwrite ? "Write (overwrite allowed): "
                                 : "Create new: ";
    return head + path + " (" + std::to_string(new_len) + " bytes)";
}

ToolResult WriteFileTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                   const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call,
            {"path", "content", "overwrite"}))
        return *err;

    std::string rel_path = call.args.value("path", "");
    std::string content  = call.args.value("content", "");
    bool overwrite       = call.args.value("overwrite", false);

    if (rel_path.empty())
        return error_result("Error: 'path' parameter is required");

    fs::path full = resolve_path(ws, rel_path);
    if (full.empty())
        return error_result("Error: path resolves outside workspace");

    bool exists = fs::exists(full);
    if (exists && !overwrite)
        return error_result("Error: file already exists: " + rel_path +
                            " (set overwrite=true to replace it, or use edit_file "
                            "for partial changes)");

    // S6.10 Task G -- block writes whose body ends in an elision marker.
    if (auto blocked = check_truncation(ws, content, rel_path, "write_file"))
        return *blocked;

    // Make sure the containing directory exists -- covers both fresh files and
    // rewrites whose directory got removed since the path was chosen.
    std::error_code ec;
    fs::create_directories(full.parent_path(), ec);
    if (ec)
        return error_result("Error: cannot create directories: " + ec.message());

    // S5.O -- atomic write so a crash between truncate and final flush leaves
    // either the previous bytes or the new bytes on disk, never a half-written
    // file. edit_file already uses the same helper.
    std::string werr = write_atomic(full, content);
    if (!werr.empty())
        return error_result("Error: " + werr);

    // The agent just authored every byte on disk -- no point forcing a
    // read_file before a follow-up edit_file. Mark as seen so the guard
    // at the top of EditFileTool::execute lets the next edit through.
    ReadTracker::instance().mark_read(full);

    spdlog::trace("write_file: {} ({}, {} bytes)", rel_path,
                  exists ? "overwrote" : "created", content.size());

    std::string msg = (exists ? "Overwrote " : "Created ") + rel_path +
                      " (" + std::to_string(content.size()) + " bytes)";
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
    std::string path = call.args.value("path", "");
    const auto& edits = call.args.value("edits", nlohmann::json::array());
    std::ostringstream ss;
    ss << "Edit " << path;
    if (edits.size() != 1)
        ss << " (" << edits.size() << " replacements)";
    ss << "\n";
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

ToolResult EditFileTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                  const std::atomic<bool>* /*cancel_flag*/)
{
    // S6.17 Task D + E -- allow both the canonical edits=[...] form AND the
    // single-edit shorthand top-level old_string / new_string / replace_all.
    if (auto err = tools::reject_unknown_keys(call,
            {"path", "edits", "old_string", "new_string", "replace_all"}))
        return *err;

    std::string rel_path = call.args.value("path", "");
    if (rel_path.empty())
        return error_result("Error: 'path' parameter is required");

    // S6.17 Task E -- single-edit shorthand. Accept top-level old_string +
    // new_string (+ optional replace_all) as a synonym for edits=[{...}] so
    // small models that miss the array shape still land their edit.
    nlohmann::json edits_array;
    if (!call.args.contains("edits")
        && call.args.contains("old_string")
        && call.args.contains("new_string")) {
        nlohmann::json single = nlohmann::json::object();
        single["old_string"] = call.args["old_string"];
        single["new_string"] = call.args["new_string"];
        if (call.args.contains("replace_all"))
            single["replace_all"] = call.args["replace_all"];
        edits_array = nlohmann::json::array({std::move(single)});
    } else if (call.args.contains("edits") && call.args["edits"].is_array()) {
        edits_array = call.args["edits"];
    } else {
        return error_result("Error: 'edits' must be an array of "
                            "{old_string, new_string, replace_all?} objects "
                            "(or pass a single edit as top-level old_string + "
                            "new_string)");
    }
    const auto& edits = edits_array;
    if (edits.empty())
        return error_result("Error: 'edits' array is empty");

    fs::path full = resolve_path(ws, rel_path);
    if (full.empty())
        return error_result("Error: path resolves outside workspace");
    if (!fs::exists(full))
        return error_result("Error: file does not exist: " + rel_path +
                            " (use write_file to create a new file)");

    // Per-workspace toggle (Settings > Tool Approvals). Default on; when the
    // user trusts the model enough to skip the confirm-read prerequisite, the
    // workspace config flips this off and edit_file goes straight to the
    // old_string match.
    bool require_read = true;
    if (Workspace* w = ws.workspace())
        require_read = w->config().agent.require_read_before_edit;
    if (require_read && !ReadTracker::instance().was_read(full))
        return error_result("Error: read the file first. Call read_file on '" +
                            rel_path +
                            "' before edit_file so the exact content is confirmed.");

    std::string buf;
    if (!slurp(full, buf))
        return error_result("Error: cannot open file for read: " + rel_path);

    // Apply every edit to an in-memory buffer. Any failure -> discard buffer,
    // file on disk is unchanged (atomicity). Later edits see earlier results.
    for (size_t i = 0; i < edits.size(); ++i) {
        const auto& e = edits[i];
        std::string old_s = e.value("old_string", "");
        std::string new_s = e.value("new_string", "");
        bool all          = e.value("replace_all", false);

        // S6.10 Task G -- screen each new_string body before applying.
        if (auto blocked = check_truncation(ws, new_s, rel_path, "edit_file"))
            return *blocked;

        std::string err = apply_single_edit(buf, old_s, new_s, all, rel_path);
        if (!err.empty()) {
            std::string where = edits.size() == 1
                ? std::string{}
                : " (edit " + std::to_string(i + 1) + "/" +
                  std::to_string(edits.size()) + ")";
            return error_result(err + where +
                                " -- no changes written (edits are atomic)");
        }
    }

    std::string werr = write_atomic(full, buf);
    if (!werr.empty())
        return error_result("Error: " + werr);

    // edit_file already required a prior read (or had the guard disabled),
    // but renew the mark so multi-edit chains in the same turn keep working
    // even if some future cleanup pass were to expire entries by recency.
    ReadTracker::instance().mark_read(full);

    spdlog::trace("edit_file: {} ({} edit{}, {} bytes)",
                  rel_path, edits.size(), edits.size() == 1 ? "" : "s",
                  buf.size());

    std::string msg = edits.size() == 1
        ? "Edited " + rel_path
        : "Applied " + std::to_string(edits.size()) + " edits to " + rel_path;
    return {true, msg, msg};
}

// -- DeleteFileTool ---------------------------------------------------------

std::string DeleteFileTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    return "WARNING: Permanently delete " + path;
}

ToolResult DeleteFileTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                    const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call, {"path"}))
        return *err;

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

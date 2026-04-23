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

#include "tools.h"
#include "index_query.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace locus {

namespace fs = std::filesystem;

// -- Helpers ----------------------------------------------------------------

// Resolve a relative path safely within the workspace root.
// Returns empty path if the resolved path escapes the workspace.
static fs::path resolve_path(const WorkspaceContext& ws, const std::string& rel)
{
    std::error_code ec;
    fs::path full = fs::canonical(ws.root / rel, ec);
    if (ec) {
        // canonical failed (file may not exist yet) — try weakly_canonical
        full = fs::weakly_canonical(ws.root / rel, ec);
        if (ec) return {};
    }

    // Ensure the resolved path is inside the workspace root
    auto root_str = ws.root.string();
    auto full_str = full.string();
    if (full_str.size() < root_str.size() ||
        full_str.compare(0, root_str.size(), root_str) != 0) {
        return {};  // path traversal attempt
    }
    return full;
}

static std::string make_relative(const WorkspaceContext& ws, const fs::path& p)
{
    return fs::relative(p, ws.root).string();
}

static ToolResult error_result(const std::string& msg)
{
    return {false, msg, msg};
}

// -- ReadFileTool -----------------------------------------------------------

std::string ReadFileTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    int offset = call.args.value("offset", 1);
    int length = call.args.value("length", 100);
    return "Read " + path + " lines " + std::to_string(offset) + "-" +
           std::to_string(offset + length - 1);
}

ToolResult ReadFileTool::execute(const ToolCall& call, const WorkspaceContext& ws)
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

    // Count total lines and read the requested range
    std::string line;
    std::ostringstream content;
    int line_num = 0;
    int lines_read = 0;
    int total_lines = 0;

    // First pass: read requested range while counting lines
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

    // Build compact LLM content
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

ToolResult WriteFileTool::execute(const ToolCall& call, const WorkspaceContext& ws)
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

ToolResult CreateFileTool::execute(const ToolCall& call, const WorkspaceContext& ws)
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

    // Create parent directories if needed
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

ToolResult DeleteFileTool::execute(const ToolCall& call, const WorkspaceContext& ws)
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

// -- ListDirectoryTool ------------------------------------------------------

ToolResult ListDirectoryTool::execute(const ToolCall& call, const WorkspaceContext& ws)
{
    std::string path = call.args.value("path", ".");
    int depth = call.args.value("depth", 0);

    if (path.empty()) path = ".";

    if (!ws.index)
        return error_result("Error: workspace index not available");

    auto entries = ws.index->list_directory(path, depth);

    std::ostringstream content;
    content << "[" << path << "] " << entries.size() << " entries\n";
    for (auto& e : entries) {
        if (e.is_directory) {
            content << "  " << e.path << "/\n";
        } else {
            content << "  " << e.path;
            if (e.size_bytes > 0)
                content << "  (" << e.size_bytes << " bytes)";
            if (!e.language.empty())
                content << "  [" << e.language << "]";
            content << "\n";
        }
    }

    std::string result = content.str();
    spdlog::trace("list_directory: {} ({} entries)", path, entries.size());
    return {true, result, result};
}

// -- SearchTextTool ---------------------------------------------------------

ToolResult SearchTextTool::execute(const ToolCall& call, const WorkspaceContext& ws)
{
    std::string query = call.args.value("query", "");
    int max_results = call.args.value("max_results", 20);

    if (query.empty())
        return error_result("Error: 'query' parameter is required");

    if (!ws.index)
        return error_result("Error: workspace index not available");

    SearchOptions opts;
    opts.max_results = max_results;

    auto results = ws.index->search_text(query, opts);

    std::ostringstream content;
    content << results.size() << " results for \"" << query << "\"\n";
    for (auto& r : results) {
        content << "  " << r.path;
        if (r.line > 0) content << ":" << r.line;
        content << " (score " << r.score << ")\n";
        if (!r.snippet.empty())
            content << "    " << r.snippet << "\n";
    }

    std::string result = content.str();
    spdlog::trace("search_text: '{}' -> {} results", query, results.size());
    return {true, result, result};
}

// -- SearchSymbolsTool ------------------------------------------------------

ToolResult SearchSymbolsTool::execute(const ToolCall& call, const WorkspaceContext& ws)
{
    std::string name_query = call.args.value("name", "");
    std::string kind     = call.args.value("kind", "");
    std::string language = call.args.value("language", "");

    if (name_query.empty())
        return error_result("Error: 'name' parameter is required");

    if (!ws.index)
        return error_result("Error: workspace index not available");

    auto results = ws.index->search_symbols(name_query, kind, language);

    std::ostringstream content;
    content << results.size() << " symbols matching \"" << name_query << "\"\n";
    for (auto& s : results) {
        content << "  " << s.kind << " " << s.name;
        if (!s.signature.empty()) content << s.signature;
        content << "  " << s.path << ":" << s.line_start;
        if (s.line_end > s.line_start)
            content << "-" << s.line_end;
        if (!s.language.empty())
            content << "  [" << s.language << "]";
        content << "\n";
    }

    std::string result = content.str();
    spdlog::trace("search_symbols: '{}' -> {} results", name_query, results.size());
    return {true, result, result};
}

// -- GetFileOutlineTool -----------------------------------------------------

ToolResult GetFileOutlineTool::execute(const ToolCall& call, const WorkspaceContext& ws)
{
    std::string path = call.args.value("path", "");

    if (path.empty())
        return error_result("Error: 'path' parameter is required");

    if (!ws.index)
        return error_result("Error: workspace index not available");

    auto entries = ws.index->get_file_outline(path);

    std::ostringstream content;
    content << "[" << path << "] outline (" << entries.size() << " entries)\n";
    for (auto& e : entries) {
        if (e.type == OutlineEntry::Heading) {
            // Indent by heading level
            for (int i = 0; i < e.level; ++i) content << "#";
            content << " " << e.text << "  (line " << e.line << ")\n";
        } else {
            content << "  " << e.kind << " " << e.text;
            if (!e.signature.empty()) content << e.signature;
            content << "  (line " << e.line << ")\n";
        }
    }

    std::string result = content.str();
    spdlog::trace("get_file_outline: {} ({} entries)", path, entries.size());
    return {true, result, result};
}

// -- RunCommandTool ---------------------------------------------------------

std::string RunCommandTool::preview(const ToolCall& call) const
{
    std::string cmd = call.args.value("command", "");
    return "Execute: " + cmd;
}

#ifdef _WIN32

ToolResult RunCommandTool::execute(const ToolCall& call, const WorkspaceContext& ws)
{
    std::string command = call.args.value("command", "");
    int timeout_ms = call.args.value("timeout_ms", 30000);

    if (command.empty())
        return error_result("Error: 'command' parameter is required");

    spdlog::trace("run_command: '{}'", command);

    // Create pipes for stdout+stderr
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return error_result("Error: failed to create pipe");

    // Don't let the read end be inherited
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError  = write_pipe;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    // Build command line: cmd /c "command"
    std::string cmd_line = "cmd /c " + command;

    BOOL created = CreateProcessA(
        nullptr,
        cmd_line.data(),
        nullptr, nullptr,
        TRUE,           // inherit handles
        CREATE_NO_WINDOW,
        nullptr,
        ws.root.string().c_str(),   // working directory
        &si, &pi
    );

    // Close write end in parent so reads will see EOF when child exits
    CloseHandle(write_pipe);

    if (!created) {
        CloseHandle(read_pipe);
        return error_result("Error: failed to create process (error " +
                            std::to_string(GetLastError()) + ")");
    }

    // Wait for process with timeout FIRST, then drain the pipe.
    // This ensures the timeout actually fires for long-running commands
    // (ReadFile on a pipe blocks until the child's write end closes).
    DWORD wait_result = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));

    DWORD exit_code = 0;
    bool timed_out = false;
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);  // wait for termination
        timed_out = true;
        exit_code = 1;
    } else {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Now drain any remaining pipe output (process is already dead)
    std::string output;
    char buf[4096];
    DWORD bytes_read;
    while (ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        output.append(buf, bytes_read);
    }
    CloseHandle(read_pipe);

    // Truncate output for LLM context
    constexpr size_t max_output = 8000;
    if (output.size() > max_output) {
        output.resize(max_output);
        output += "\n... (output truncated)";
    }

    std::ostringstream content;
    if (timed_out)
        content << "[TIMEOUT after " << timeout_ms << "ms]\n";
    content << "[exit code: " << exit_code << "]\n";
    content << output;

    std::string result = content.str();

    spdlog::trace("run_command: exit={} output={} bytes{}", exit_code,
                  output.size(), timed_out ? " (timed out)" : "");

    return {exit_code == 0 && !timed_out, result, result};
}

#else

ToolResult RunCommandTool::execute(const ToolCall& call, const WorkspaceContext& ws)
{
    // POSIX stub — not implemented for Windows-first development
    return error_result("Error: run_command not implemented on this platform");
}

#endif

// -- Factory ----------------------------------------------------------------

void register_builtin_tools(IToolRegistry& registry)
{
    registry.register_tool(std::make_unique<ReadFileTool>());
    registry.register_tool(std::make_unique<WriteFileTool>());
    registry.register_tool(std::make_unique<CreateFileTool>());
    registry.register_tool(std::make_unique<DeleteFileTool>());
    registry.register_tool(std::make_unique<ListDirectoryTool>());
    registry.register_tool(std::make_unique<SearchTextTool>());
    registry.register_tool(std::make_unique<SearchSymbolsTool>());
    registry.register_tool(std::make_unique<GetFileOutlineTool>());
    registry.register_tool(std::make_unique<RunCommandTool>());
}

} // namespace locus

#include "tools/index_tools.h"
#include "tools/shared.h"

#include "core/workspace.h"
#include "core/workspace_services.h"
#include "index/glob_match.h"
#include "index/index_query.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace locus {

namespace fs = std::filesystem;
using tools::error_result;

// -- ListDirectoryTool ------------------------------------------------------

namespace {

// Extensions the model should not bother reading. Header-internal -- kept narrow
// on purpose so anything outside the list falls through to [unindexed], inviting
// `read_file` if the model suspects it might still be text.
bool is_binary_extension(std::string ext)
{
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::unordered_set<std::string> k_exts = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico", ".bmp", ".tif", ".tiff",
        ".pdf", ".zip", ".tar", ".gz", ".bz2", ".xz", ".7z", ".rar",
        ".exe", ".dll", ".so", ".dylib", ".a", ".lib", ".o", ".obj", ".pdb",
        ".ttf", ".otf", ".woff", ".woff2",
        ".mp3", ".mp4", ".wav", ".mov", ".mkv", ".avi", ".flac", ".ogg",
        ".bin",
    };
    return k_exts.count(ext) != 0;
}

// True if the workspace-relative path matches any configured exclude glob.
bool path_excluded(const std::string& rel_str,
                   const std::vector<std::string>& patterns)
{
    for (const auto& p : patterns) {
        if (glob_match(p, rel_str)) return true;
    }
    return false;
}

// Directories need a second probe because patterns like ".git/**" only match
// paths *under* the directory, not the directory itself.
bool dir_excluded(const std::string& rel_str,
                  const std::vector<std::string>& patterns)
{
    if (path_excluded(rel_str, patterns)) return true;
    std::string probe = rel_str.empty() ? std::string("x") : rel_str + "/x";
    return path_excluded(probe, patterns);
}

struct IndexedMeta {
    int64_t     size_bytes = 0;
    std::string language;
    bool        is_binary  = false;
    bool        present    = false;
};

struct WalkContext {
    fs::path                                 abs_root;
    const std::vector<std::string>*          excludes      = nullptr;
    int64_t                                  size_cap_bytes = 0;  // 0 = no cap
    const std::unordered_map<std::string, IndexedMeta>* index_meta = nullptr;
    int                                      max_entries   = 200;
};

struct WalkOut {
    std::vector<std::string> lines;
    int truncated = 0;
};

// Format the per-file line. Annotation precedence:
//   - binary extension                                   -> [binary]
//   - on-disk size exceeds indexer size cap              -> [oversized]
//   - present in index with a language                   -> [<language>]
//   - present in index but indexer flagged as binary     -> [binary]
//   - anything else                                      -> [unindexed]
std::string format_file_line(const std::string& rel_str,
                             int64_t            on_disk_size,
                             const std::string& ext,
                             const WalkContext& cx)
{
    std::string annotation;

    if (is_binary_extension(ext)) {
        annotation = "[binary]";
    } else if (cx.size_cap_bytes > 0 && on_disk_size > cx.size_cap_bytes) {
        annotation = "[oversized]";
    } else {
        auto it = cx.index_meta->find(rel_str);
        if (it != cx.index_meta->end() && it->second.present) {
            if (!it->second.is_binary && !it->second.language.empty()) {
                annotation = "[" + it->second.language + "]";
            } else if (it->second.is_binary) {
                annotation = "[binary]";
            } else {
                // Indexed text without a language tag (e.g. plain `.txt`).
                annotation = "[text]";
            }
        } else {
            annotation = "[unindexed]";
        }
    }

    std::ostringstream line;
    line << "  " << rel_str
         << "  (" << on_disk_size << " bytes)  "
         << annotation;
    return line.str();
}

// Depth-first walk; emits lines into `out`. `depth_remaining` is the number of
// recursion levels left below the current directory (0 = immediate children
// only).
void walk_dir(const fs::path&     abs_dir,
              const std::string&  rel_dir,           // forward-slash form, empty for root
              int                 depth_remaining,
              WalkContext&        cx,
              WalkOut&            out)
{
    std::error_code ec;
    fs::directory_iterator it(abs_dir,
        fs::directory_options::skip_permission_denied, ec);
    if (ec) return;

    // Collect + sort for stable output: directories first, then files, both
    // case-insensitive by name (matches IndexQuery::list_directory's display
    // order well enough for the LLM).
    std::vector<fs::directory_entry> entries;
    for (const auto& de : it) entries.push_back(de);

    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  std::error_code e1, e2;
                  bool ad = a.is_directory(e1);
                  bool bd = b.is_directory(e2);
                  if (ad != bd) return ad > bd;  // dirs first
                  return a.path().filename().string() < b.path().filename().string();
              });

    for (const auto& de : entries) {
        std::error_code e1;
        bool is_dir = de.is_directory(e1);
        // Symlinks: do not follow. directory_iterator does not recurse by
        // default; symlinks-to-dirs will be reported as directories but we
        // won't descend into them because we control the recursion ourselves.
        if (de.is_symlink(e1)) is_dir = false;

        fs::path rel_path = de.path().lexically_relative(cx.abs_root);
        std::string rel_str = rel_path.generic_string();
        if (rel_str.empty() || rel_str == ".") continue;

        if (is_dir) {
            if (dir_excluded(rel_str, *cx.excludes)) continue;

            if (static_cast<int>(out.lines.size()) >= cx.max_entries) {
                ++out.truncated;
                continue;
            }
            out.lines.push_back("  " + rel_str + "/");

            if (depth_remaining > 0) {
                walk_dir(de.path(), rel_str, depth_remaining - 1, cx, out);
            }
        } else {
            if (path_excluded(rel_str, *cx.excludes)) continue;

            if (static_cast<int>(out.lines.size()) >= cx.max_entries) {
                ++out.truncated;
                continue;
            }

            int64_t sz = 0;
            std::error_code e2;
            sz = static_cast<int64_t>(de.file_size(e2));
            if (e2) sz = 0;

            std::string ext = rel_path.extension().generic_string();
            out.lines.push_back(format_file_line(rel_str, sz, ext, cx));
        }
    }
}

} // namespace

std::string ListDirectoryTool::preview(const ToolCall& call) const
{
    std::string path = call.args.value("path", "");
    if (path.empty() || path == ".") path = "(workspace root)";
    int depth = call.args.value("depth", 0);
    std::string out = "list_directory: " + path;
    if (depth > 0) out += "  depth=" + std::to_string(depth);
    return out;
}

ToolResult ListDirectoryTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                      const std::atomic<bool>* /*cancel_flag*/)
{
    std::string path = call.args.value("path", "");
    int depth = call.args.value("depth", 0);
    int max_entries = call.args.value("max_entries", 200);
    if (max_entries <= 0) max_entries = 200;
    if (depth < 0) depth = 0;

    // Normalize: "." means root, same as empty.
    if (path == "." || path == "./") path.clear();

    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    // We need the workspace for exclude patterns + size cap. Without it (e.g.
    // unit-test FakeWorkspaceServices), fall back to no excludes / no size cap.
    Workspace* wsp = ws.workspace();
    std::vector<std::string> excludes;
    int64_t size_cap_bytes = 0;
    if (wsp) {
        excludes = wsp->config().exclude_patterns;
        if (wsp->config().max_file_size_kb > 0) {
            size_cap_bytes = static_cast<int64_t>(wsp->config().max_file_size_kb) * 1024;
        }
    }

    // Resolve the requested directory under the workspace root.
    fs::path abs_root = ws.root();
    fs::path abs_dir  = path.empty() ? abs_root : (abs_root / path);

    std::error_code ec;
    if (!fs::exists(abs_dir, ec) || !fs::is_directory(abs_dir, ec)) {
        return error_result("Error: path is not a directory: " +
                            (path.empty() ? "." : path));
    }

    // Build the cross-reference map once. The index's `list_directory` already
    // knows file size / language / binary-flag; we hand each filesystem entry
    // through this hashmap for the annotation pass.
    std::unordered_map<std::string, IndexedMeta> index_meta;
    {
        // A generous depth keeps every indexed file under the requested path
        // visible to the annotation pass regardless of nesting.
        constexpr int k_lookup_depth = 1'000'000;
        auto rows = idx->list_directory(path, k_lookup_depth);
        for (const auto& fe : rows) {
            if (fe.is_directory) continue;
            IndexedMeta m;
            m.size_bytes = fe.size_bytes;
            m.language   = fe.language;
            m.is_binary  = fe.is_binary;
            m.present    = true;
            index_meta.emplace(fe.path, std::move(m));
        }
    }

    WalkContext cx;
    cx.abs_root       = abs_root;
    cx.excludes       = &excludes;
    cx.size_cap_bytes = size_cap_bytes;
    cx.index_meta     = &index_meta;
    cx.max_entries    = max_entries;

    WalkOut out;
    walk_dir(abs_dir, path, depth, cx, out);

    std::ostringstream content;
    std::string display_path = path.empty() ? "." : path;
    content << "[" << display_path << "] " << out.lines.size() << " entries\n";
    for (const auto& line : out.lines) content << line << "\n";
    if (out.truncated > 0) {
        content << "[" << out.truncated
                << " more entries truncated; refine path or use search]\n";
    }

    std::string result = content.str();
    spdlog::trace("list_directory: {} ({} entries, {} truncated)",
                  display_path, out.lines.size(), out.truncated);
    return {true, result, result};
}

// -- GetFileOutlineTool -----------------------------------------------------

bool GetFileOutlineTool::available(IWorkspaceServices& ws) const
{
    auto* w = ws.workspace();
    return w ? w->config().capabilities.code_aware_search : true;
}

std::string GetFileOutlineTool::preview(const ToolCall& call) const
{
    return "get_file_outline: " + call.args.value("path", "");
}

ToolResult GetFileOutlineTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                        const std::atomic<bool>* /*cancel_flag*/)
{
    std::string path = call.args.value("path", "");

    if (path.empty())
        return error_result("Error: 'path' parameter is required");

    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    auto entries = idx->get_file_outline(path);

    std::ostringstream content;
    content << "[" << path << "] outline (" << entries.size() << " entries)\n";
    for (auto& e : entries) {
        if (e.type == OutlineEntry::Heading) {
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

} // namespace locus

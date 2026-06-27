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
    int64_t     line_count = 0;
    std::string language;
    bool        is_binary  = false;
    bool        present    = false;
};

// S6.18 F -- Windows-style case-insensitive form used as a fallback lookup
// key. The indexer stores paths with whatever case the FS produced when the
// file was indexed; the walk produces paths with whatever case
// `directory_iterator` reports today. On Windows these can diverge (renames
// across casings, watcher event vs. fresh-walk delivery, etc.) and the
// case-sensitive `unordered_map::find` then misses, surfacing as
// `[unindexed]` for a file that IS in the index. Lowercase ASCII fold is
// enough -- non-ASCII paths on Windows are rare in our test corpora and the
// fold is a one-direction normalisation, not a search.
std::string to_lower_lookup_key(std::string s)
{
    for (auto& c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        c = static_cast<char>(std::tolower(uc));
    }
    return s;
}

struct WalkContext {
    fs::path                                 abs_root;
    const std::vector<std::string>*          excludes      = nullptr;
    int64_t                                  size_cap_bytes = 0;  // 0 = no cap
    const std::unordered_map<std::string, IndexedMeta>* index_meta = nullptr;
    // S6.18 F -- secondary case-insensitive map. Filled alongside index_meta;
    // consulted only when the primary find misses. Keys lowercased via
    // to_lower_lookup_key.
    const std::unordered_map<std::string, const IndexedMeta*>* index_meta_ci = nullptr;
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

    int64_t indexed_line_count = 0;
    if (is_binary_extension(ext)) {
        annotation = "[binary]";
    } else if (cx.size_cap_bytes > 0 && on_disk_size > cx.size_cap_bytes) {
        annotation = "[oversized]";
    } else {
        const IndexedMeta* meta = nullptr;
        auto it = cx.index_meta->find(rel_str);
        if (it != cx.index_meta->end() && it->second.present) {
            meta = &it->second;
        } else if (cx.index_meta_ci) {
            // Case-insensitive fallback. Windows-only failure mode in
            // practice; the lowercase fold is cheap enough to apply
            // unconditionally so the same code holds on case-sensitive
            // filesystems (where the primary find never misses for a
            // mismatched-case path because that path doesn't exist).
            auto ci_it = cx.index_meta_ci->find(to_lower_lookup_key(rel_str));
            if (ci_it != cx.index_meta_ci->end() && ci_it->second
                && ci_it->second->present) {
                meta = ci_it->second;
            }
        }
        if (meta) {
            if (!meta->is_binary && !meta->language.empty()) {
                annotation = "[" + meta->language + "]";
                indexed_line_count = meta->line_count;
            } else if (meta->is_binary) {
                annotation = "[binary]";
            } else {
                // Indexed text without a language tag (e.g. plain `.txt`).
                annotation = "[text]";
                indexed_line_count = meta->line_count;
            }
        } else {
            annotation = "[unindexed]";
        }
    }

    std::ostringstream line;
    line << "  " << rel_str
         << "  (" << on_disk_size << " bytes)  "
         << annotation;
    // Append [N lines] for indexed text rows. Binary / oversized / unindexed
    // entries skip this; legacy rows (line_count NULL -> 0) also skip so the
    // annotation only ever appears when it's real.
    if (indexed_line_count > 0) {
        line << "  [" << indexed_line_count << " lines]";
    }
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
    int depth = static_cast<int>(tools::coerce_int(call.args, "depth", 0));
    std::string out = "list_directory: " + path;
    if (depth > 0) out += "  depth=" + std::to_string(depth);
    return out;
}

ToolResult ListDirectoryTool::execute(const ToolCall& call, IWorkspaceServices& ws,
                                      const std::atomic<bool>* /*cancel_flag*/)
{
    if (auto err = tools::reject_unknown_keys(call,
            {"path", "depth", "max_entries"}, this))
        return *err;

    std::string path = call.args.value("path", "");
    int depth = static_cast<int>(tools::coerce_int(call.args, "depth", 0));
    int max_entries = static_cast<int>(tools::coerce_int(call.args, "max_entries", 200));
    if (max_entries <= 0) max_entries = 200;
    if (depth < 0) depth = 0;

    // Normalize: "." means root, same as empty.
    if (path == "." || path == "./") path.clear();

    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    // S6.2 -- ZIM mode: there is NO real filesystem to walk. Enumerate article
    // paths straight from the index (the only source of truth) and present them
    // as a browseable listing. Directories are the synthetic path prefixes the
    // index already derives. Origin marker reminds the model these are
    // untrusted Wikipedia articles.
    if (auto* zw = ws.workspace(); zw && zw->zim_mode()) {
        auto rows = idx->list_directory(path, depth > 0 ? depth : 1);
        std::ostringstream content;
        std::string display_path = path.empty() ? "." : path;
        int shown = 0, truncated = 0;
        std::ostringstream lines;
        for (const auto& fe : rows) {
            if (shown >= max_entries) { ++truncated; continue; }
            lines << "  " << fe.path;
            if (fe.is_directory) lines << "/";
            else lines << " [wikipedia, untrusted]";
            lines << "\n";
            ++shown;
        }
        content << "[" << display_path << "] " << shown
                << " entries (ZIM archive, read-only)\n" << lines.str();
        if (truncated > 0)
            content << "[" << truncated
                    << " more truncated; use search_text to find articles]\n";
        std::string result = content.str();
        return {true, result, result};
    }

    // We need the workspace for exclude patterns + size cap. Without it (e.g.
    // unit-test FakeWorkspaceServices), fall back to no excludes / no size cap.
    Workspace* wsp = ws.workspace();
    std::vector<std::string> excludes;
    int64_t size_cap_bytes = 0;
    if (wsp) {
        excludes = wsp->config().index.exclude_patterns;
        if (wsp->config().index.max_file_size_kb > 0) {
            size_cap_bytes = static_cast<int64_t>(wsp->config().index.max_file_size_kb) * 1024;
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
    // S6.18 F -- case-insensitive sibling for the Windows-rename failure mode.
    // Points into index_meta values; both maps share lifetime with this scope.
    std::unordered_map<std::string, const IndexedMeta*> index_meta_ci;
    {
        // A generous depth keeps every indexed file under the requested path
        // visible to the annotation pass regardless of nesting.
        constexpr int k_lookup_depth = 1'000'000;
        auto rows = idx->list_directory(path, k_lookup_depth);
        for (const auto& fe : rows) {
            if (fe.is_directory) continue;
            IndexedMeta m;
            m.size_bytes = fe.size_bytes;
            m.line_count = fe.line_count;
            m.language   = fe.language;
            m.is_binary  = fe.is_binary;
            m.present    = true;
            auto [it, inserted] = index_meta.emplace(fe.path, std::move(m));
            index_meta_ci.emplace(to_lower_lookup_key(fe.path), &it->second);
        }
    }

    // S6.17 Task F -- diagnostic. When the index map is empty but the FS
    // walk below will find files, the most likely cause is a path-key shape
    // mismatch (forward-slash vs backslash, case, etc.) between insert and
    // lookup. Sample the first five file keys we DO have so a developer
    // staring at the trace log can confirm the format at a glance.
    if (index_meta.empty()) {
        auto sample = idx->list_directory("", 1'000'000);
        std::ostringstream keys;
        int shown = 0;
        for (const auto& fe : sample) {
            if (fe.is_directory) continue;
            if (shown++ > 0) keys << ", ";
            keys << "'" << fe.path << "'";
            if (shown >= 5) break;
        }
        spdlog::trace("list_directory: IndexedMeta lookup miss for rel='{}'; "
                      "files-table keys (sample of {}): [{}]",
                      path, shown, keys.str());
    }

    WalkContext cx;
    cx.abs_root       = abs_root;
    cx.excludes       = &excludes;
    cx.size_cap_bytes = size_cap_bytes;
    cx.index_meta     = &index_meta;
    cx.index_meta_ci  = &index_meta_ci;
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
    if (auto err = tools::reject_unknown_keys(call, {"path"}, this))
        return *err;

    std::string path = call.args.value("path", "");

    if (path.empty())
        return tools::missing_required_arg(*this, "path",
            "the workspace-relative path to outline");

    auto* idx = ws.index();
    if (!idx)
        return error_result("Error: workspace index not available");

    auto entries = idx->get_file_outline(path);
    auto line_count = idx->get_file_line_count(path);

    // When the index returns nothing, distinguish the failure modes so the
    // agent doesn't see a silent "0 entries" for four different problems.
    // See tests/test_outline_race.cpp for the cases.
    if (entries.empty()) {
        fs::path abs_root = ws.root();
        fs::path abs_path = abs_root / path;
        std::error_code ec;
        bool exists_on_disk = fs::exists(abs_path, ec) && fs::is_regular_file(abs_path, ec);

        if (!exists_on_disk) {
            return error_result(
                "Error: file not found at '" + path + "'. Check the path is "
                "relative to the workspace root.");
        }

        // File is on disk but no symbols are in the index. Could be (a) a
        // pending-index race -- common after a fresh write_file by an external
        // editor -- or (b) the file is genuinely empty / unsupported. Surface
        // this honestly instead of returning a misleading "0 entries" line
        // that looks identical to "extractor saw the file and found nothing".
        int64_t on_disk_size = static_cast<int64_t>(fs::file_size(abs_path, ec));
        if (ec) on_disk_size = 0;

        int64_t size_cap_bytes = 0;
        if (auto* wsp = ws.workspace()) {
            int kb = wsp->config().index.max_file_size_kb;
            if (kb > 0) size_cap_bytes = static_cast<int64_t>(kb) * 1024;
        }

        std::ostringstream msg;
        msg << "[" << path << "]";
        if (line_count) msg << " (" << *line_count << " lines)";
        msg << " no symbols or headings found in the index.\n";
        if (size_cap_bytes > 0 && on_disk_size > size_cap_bytes) {
            msg << "File is " << on_disk_size << " bytes -- above the index "
                << "size cap (" << size_cap_bytes << " bytes); not indexed.\n"
                << "Use read_file with offset/length to read it in pages.\n";
        } else {
            msg << "File is present on disk (" << on_disk_size << " bytes) "
                << "but the index has no symbols for it. Possible causes:\n"
                << "  - file was just written and indexing has not caught up yet (retry)\n"
                << "  - language is not in the extractor table (C/C++, Python, JS/TS, "
                << "Go, Rust, Java, C#, Ruby, PHP, Bash, Swift, Kotlin)\n"
                << "  - file genuinely has no top-level declarations\n"
                << "Try `search` with mode=text or mode=regex if you need lines, "
                << "or `read_file` to see the body directly.\n";
        }
        std::string body = msg.str();
        spdlog::trace("get_file_outline: {} -> empty (exists_on_disk={}, size={})",
                      path, exists_on_disk, on_disk_size);
        return {true, body, body};
    }

    std::ostringstream content;
    content << "[" << path << "]";
    if (line_count) content << " (" << *line_count << " lines)";
    content << " outline (" << entries.size() << " entries)\n";
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

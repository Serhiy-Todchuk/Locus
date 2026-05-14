#include "prompt_templates.h"
#include "core/global_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace locus {

namespace fs = std::filesystem;

namespace {

std::string read_whole_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim_copy(std::string_view s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// Split a `[a, b, "c, d"]` YAML inline list into a vector. Tolerant of
// whitespace and quote characters; commas inside quotes are preserved.
std::vector<std::string> parse_inline_list(std::string_view raw)
{
    std::vector<std::string> out;
    // Strip leading '[' and trailing ']' if present.
    std::string s(raw);
    auto t = trim_copy(s);
    if (!t.empty() && t.front() == '[') t.erase(0, 1);
    if (!t.empty() && t.back()  == ']') t.pop_back();

    std::string cur;
    bool in_quote = false;
    char quote = 0;
    for (char c : t) {
        if (in_quote) {
            if (c == quote) in_quote = false;
            else cur += c;
            continue;
        }
        if (c == '"' || c == '\'') { in_quote = true; quote = c; continue; }
        if (c == ',') {
            auto v = trim_copy(cur);
            if (!v.empty()) out.push_back(std::move(v));
            cur.clear();
            continue;
        }
        cur += c;
    }
    auto v = trim_copy(cur);
    if (!v.empty()) out.push_back(std::move(v));
    return out;
}

// Strip leading/trailing matched single or double quotes from a YAML value.
std::string unquote(std::string s)
{
    s = trim_copy(s);
    if (s.size() >= 2
        && ((s.front() == '"' && s.back() == '"')
            || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

} // namespace

// -- Parse / substitute helpers ----------------------------------------------

PromptTemplate PromptTemplateRegistry::parse_file_contents(
    const std::string& contents, const std::string& name)
{
    PromptTemplate t;
    t.name = name;

    // Detect frontmatter only when the *first* line is exactly `---`.
    // Anything else (including leading blank lines) is treated as body.
    size_t pos = 0;
    bool has_frontmatter = false;
    if (contents.size() >= 3 && contents.compare(0, 3, "---") == 0) {
        // Must be a full line: `---\n` or `---\r\n`.
        size_t after = 3;
        if (after < contents.size() && contents[after] == '\r') ++after;
        if (after < contents.size() && contents[after] == '\n') {
            has_frontmatter = true;
            pos = after + 1;
        }
    }

    if (has_frontmatter) {
        // Find the closing `---` on its own line.
        while (pos < contents.size()) {
            size_t eol = contents.find('\n', pos);
            std::string line = (eol == std::string::npos)
                ? contents.substr(pos)
                : contents.substr(pos, eol - pos);
            // Trim trailing CR
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line == "---") {
                pos = (eol == std::string::npos) ? contents.size() : eol + 1;
                break;
            }

            // Parse `key: value`
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = trim_copy(std::string_view(line).substr(0, colon));
                std::string val = trim_copy(std::string_view(line).substr(colon + 1));

                if (key == "description") {
                    t.description = unquote(std::move(val));
                } else if (key == "args") {
                    t.args = parse_inline_list(val);
                }
                // Other keys: ignored (forward-compat for future frontmatter
                // additions; loud rejection would break templates authored
                // for a newer Locus).
            }

            if (eol == std::string::npos) {
                pos = contents.size();
                break;
            }
            pos = eol + 1;
        }
    }

    t.body = contents.substr(pos);
    return t;
}

std::string PromptTemplateRegistry::substitute(
    std::string_view body,
    const std::vector<std::string>& positional,
    const std::unordered_map<std::string, std::string>& kwargs)
{
    std::string out;
    out.reserve(body.size());

    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];

        // `{{` -> literal `{`; `}}` -> literal `}`.
        if (c == '{' && i + 1 < body.size() && body[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        if (c == '}' && i + 1 < body.size() && body[i + 1] == '}') {
            out += '}';
            ++i;
            continue;
        }

        if (c != '{') {
            out += c;
            continue;
        }

        // Scan for matching `}`. If the placeholder spans a newline or runs
        // to EOF we treat it as literal text (no surprise multiline gluing).
        size_t close = body.find_first_of("}\n", i + 1);
        if (close == std::string::npos || body[close] != '}') {
            out += c;
            continue;
        }
        std::string key(body.substr(i + 1, close - i - 1));
        std::string key_trim = trim_copy(key);

        if (key_trim.empty()) {
            // `{}` -- not a valid placeholder. Pass through verbatim.
            out += '{';
            out += key;
            out += '}';
            i = close;
            continue;
        }

        // Positional: `{0}`, `{1}`, ...
        bool numeric = std::all_of(key_trim.begin(), key_trim.end(),
            [](unsigned char ch) { return std::isdigit(ch); });
        if (numeric) {
            size_t idx = std::stoul(key_trim);
            if (idx < positional.size()) {
                out += positional[idx];
            } else {
                // Missing positional -- render literal so the user sees it.
                out += '{';
                out += key_trim;
                out += '}';
            }
            i = close;
            continue;
        }

        // Named: `{key}`
        auto it = kwargs.find(key_trim);
        if (it != kwargs.end()) {
            out += it->second;
        } else {
            out += '{';
            out += key_trim;
            out += '}';
        }
        i = close;
    }

    return out;
}

// -- Default global directory -------------------------------------------------

fs::path PromptTemplateRegistry::default_global_dir()
{
    // S5.M -- resolves to ~/.locus/prompts (post-migration).
    return global_paths::prompts_dir();
}

// -- Registry -----------------------------------------------------------------

PromptTemplateRegistry::PromptTemplateRegistry(fs::path project_dir,
                                               fs::path global_dir)
    : project_dir_(std::move(project_dir))
    , global_dir_(std::move(global_dir))
{
    reload();
}

int PromptTemplateRegistry::reload()
{
    std::lock_guard lock(mu_);
    project_entries_.clear();
    global_entries_.clear();

    auto scan = [](const fs::path& dir, bool is_project,
                   std::unordered_map<std::string, Entry>& out) {
        if (dir.empty()) return;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;

        for (auto& de : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!de.is_regular_file()) continue;
            auto p = de.path();
            if (p.extension() != ".md" && p.extension() != ".markdown") continue;

            std::string name = p.stem().string();
            if (name.empty()) continue;

            std::string contents = read_whole_file(p);
            PromptTemplate tpl = PromptTemplateRegistry::parse_file_contents(
                contents, name);
            tpl.source_path = p;
            tpl.is_project  = is_project;

            std::error_code mt_ec;
            Entry e;
            e.tpl   = std::move(tpl);
            e.mtime = fs::last_write_time(p, mt_ec);
            out[name] = std::move(e);
        }
    };

    scan(project_dir_, /*is_project=*/true,  project_entries_);
    scan(global_dir_,  /*is_project=*/false, global_entries_);

    int total = static_cast<int>(project_entries_.size());
    for (auto& [n, _] : global_entries_) {
        if (!project_entries_.count(n)) ++total;
    }
    spdlog::info("PromptTemplateRegistry: {} project + {} global = {} unique",
                 project_entries_.size(), global_entries_.size(), total);
    return total;
}

void PromptTemplateRegistry::refresh_locked() const
{
    auto refresh = [](std::unordered_map<std::string, Entry>& entries) {
        for (auto& [name, e] : entries) {
            std::error_code ec;
            auto now = fs::last_write_time(e.tpl.source_path, ec);
            if (ec) continue;
            if (now == e.mtime) continue;

            std::string contents = read_whole_file(e.tpl.source_path);
            PromptTemplate tpl = PromptTemplateRegistry::parse_file_contents(
                contents, name);
            tpl.source_path = e.tpl.source_path;
            tpl.is_project  = e.tpl.is_project;
            e.tpl   = std::move(tpl);
            e.mtime = now;
        }
    };
    refresh(project_entries_);
    refresh(global_entries_);
}

std::vector<PromptTemplate> PromptTemplateRegistry::list() const
{
    std::lock_guard lock(mu_);
    refresh_locked();

    std::vector<PromptTemplate> out;
    out.reserve(project_entries_.size() + global_entries_.size());

    // Project first, sorted by name.
    std::vector<const Entry*> proj;
    proj.reserve(project_entries_.size());
    for (auto& [_, e] : project_entries_) proj.push_back(&e);
    std::sort(proj.begin(), proj.end(),
              [](const Entry* a, const Entry* b) { return a->tpl.name < b->tpl.name; });
    for (auto* e : proj) out.push_back(e->tpl);

    // Then global, skipping names already in project (project wins).
    std::vector<const Entry*> glob;
    glob.reserve(global_entries_.size());
    for (auto& [name, e] : global_entries_) {
        if (project_entries_.count(name)) continue;
        glob.push_back(&e);
    }
    std::sort(glob.begin(), glob.end(),
              [](const Entry* a, const Entry* b) { return a->tpl.name < b->tpl.name; });
    for (auto* e : glob) out.push_back(e->tpl);

    return out;
}

bool PromptTemplateRegistry::has(const std::string& name) const
{
    std::lock_guard lock(mu_);
    return project_entries_.count(name) != 0
        || global_entries_.count(name)  != 0;
}

std::optional<PromptTemplate>
PromptTemplateRegistry::find(const std::string& name) const
{
    std::lock_guard lock(mu_);
    refresh_locked();

    auto p = project_entries_.find(name);
    if (p != project_entries_.end()) return p->second.tpl;
    auto g = global_entries_.find(name);
    if (g != global_entries_.end()) return g->second.tpl;
    return std::nullopt;
}

std::string PromptTemplateRegistry::expand(
    const std::string& name,
    const std::vector<std::string>& positional,
    const std::unordered_map<std::string, std::string>& kwargs) const
{
    auto t = find(name);
    if (!t) {
        throw std::runtime_error("prompt template not found: " + name);
    }
    return substitute(t->body, positional, kwargs);
}

} // namespace locus

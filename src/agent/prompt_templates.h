#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace locus {

// One parsed prompt template (S4.X). Lives on disk as
// `<dir>/<name>.md`, optionally with a tiny YAML-subset frontmatter:
//
//   ---
//   description: Generate a standup from this week's git activity
//   args: [target_date]
//   ---
//   <markdown body with {0} / {key} placeholders>
//
// Templates are pure text expansion -- no execution, no tool calls, no LLM
// passes. The expanded text becomes the user's message; the agent then
// decides whether to call tools as it would for any user message.
struct PromptTemplate {
    std::string              name;          // basename without `.md`
    std::string              description;   // from frontmatter (optional)
    std::vector<std::string> args;          // declared positional names (optional)
    std::string              body;          // markdown body, frontmatter stripped
    std::filesystem::path    source_path;   // for help / debug
    bool                     is_project = false;  // false = global
};

// Scans `<project_dir>/<name>.md` and `<global_dir>/<name>.md`, caches each
// parsed template by name (project overrides global on collision), and serves
// `expand()` for runtime substitution.
//
// Lazy mtime check on every list/find/expand re-reads any file that changed
// on disk since the last read. Explicit `reload()` does a full directory
// scan, picking up new files and dropping removed ones. Both are cheap.
//
// Thread-safe: a single mutex guards the cache and the directory state.
// Callers are expected to be infrequent (one scan per workspace open + one
// per slash command).
class PromptTemplateRegistry {
public:
    PromptTemplateRegistry(std::filesystem::path project_dir,
                           std::filesystem::path global_dir);

    // Drop the cache and re-scan both directories. Cheap; OK to call on every
    // workspace switch or `/reload`. Returns the number of templates loaded.
    int reload();

    // All currently-known templates, project entries first (then global,
    // skipping any names already covered by a project entry). Lazy mtime
    // check picks up edits to known files since the last call.
    std::vector<PromptTemplate> list() const;

    // True if a template with this name exists (project or global).
    bool has(const std::string& name) const;

    // Find by name; project wins on collision. Returns nullopt if absent.
    std::optional<PromptTemplate> find(const std::string& name) const;

    // Expand `name` with positional + named args. Missing references render
    // as the literal placeholder (e.g. `{2}` or `{missing}`) so the user
    // sees what wasn't resolved.
    //
    // Escapes: a literal `{` is written `{{` and a literal `}` is `}}` in
    // the template body. (Mirrors std::format / Python format mini-language.)
    //
    // Throws std::runtime_error if no template by that name. Empty positional
    // / kwargs are fine -- a template with no placeholders just returns its
    // body unchanged.
    std::string expand(
        const std::string& name,
        const std::vector<std::string>& positional,
        const std::unordered_map<std::string, std::string>& kwargs) const;

    // Pure substitution helper exposed for tests + reuse. Same escape rules
    // as `expand`. Caller owns the body string.
    static std::string substitute(
        std::string_view body,
        const std::vector<std::string>& positional,
        const std::unordered_map<std::string, std::string>& kwargs);

    // Parse a raw `.md` file's contents into a PromptTemplate. Frontmatter
    // (between leading `---` markers) is consumed; the remainder is `body`.
    // Exposed for unit tests; production code goes through `reload()`.
    static PromptTemplate parse_file_contents(const std::string& contents,
                                              const std::string& name);

    // Conventional global directory: %APPDATA%/Locus/prompts on Windows,
    // $XDG_CONFIG_HOME/locus/prompts (or ~/.locus/prompts) on Unix. Returns
    // an empty path if no usable env var was set (caller can decide whether
    // to fall back to a home-relative path).
    static std::filesystem::path default_global_dir();

private:
    struct Entry {
        PromptTemplate     tpl;
        std::filesystem::file_time_type mtime{};
    };

    // Refresh known entries' bodies if their on-disk mtime has changed.
    // Called by const accessors under the mutex.
    void refresh_locked() const;

    std::filesystem::path project_dir_;
    std::filesystem::path global_dir_;

    // mutable so const accessors can run the lazy mtime refresh.
    mutable std::mutex                                       mu_;
    mutable std::unordered_map<std::string, Entry>           project_entries_;
    mutable std::unordered_map<std::string, Entry>           global_entries_;
};

} // namespace locus

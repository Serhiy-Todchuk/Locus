#pragma once

// S6.16 -- Endpoint Profiles. A persisted, global (per-user, never per-workspace)
// list of OpenAI-compatible LLM sources the user can flip between mid-session.
//
// All six seed profiles are OpenAI-compatible -- the `api_key` + `extra_headers`
// plumbing on LLMConfig / OpenAiTransport is the entire transport layer; no new
// client classes. "Claude" lands as just another profile pointing at a local
// OpenAI-compat proxy (LiteLLM / claude-code-proxy); native Anthropic
// /v1/messages is out of scope.

#include "llm/llm_client.h"   // LLMConfig

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace locus {

// One configured LLM source. Mirrors the on-disk JSON shape one-for-one.
struct EndpointProfile {
    std::string name;                 // user-visible label, unique within store
    std::string base_url;             // e.g. https://integrate.api.nvidia.com/v1
    std::string api_key;              // empty for local-no-auth endpoints
    std::string default_model;        // "" = use server default / query_model_info
    int         default_context_limit = 0;          // 0 = ask the server
    std::string tool_format = "auto";                // overrides workspace tool_format on activation
    std::map<std::string, std::string> extra_headers; // future-proofing, empty default
    bool        builtin = false;                     // seed profiles; survive store regeneration
};

// Layer a profile's endpoint identity into `cfg`.
//
//   force=false (startup resolution): each endpoint-identity field
//     (base_url / model / context_limit / tool_format) is taken from the
//     profile ONLY when `cfg` is still at its unset/default sentinel, so a
//     non-default `.locus/config.json llm.endpoint` or a caller seed wins --
//     the one-way ratchet from the stage doc.
//   force=true (explicit hot-swap): the profile overlays endpoint identity
//     unconditionally (the user picked this profile). model / context_limit
//     are taken verbatim from the profile defaults (empty / 0 -> the caller
//     re-detects via query_model_info afterwards).
//
// `api_key` / `extra_headers` have no legacy equivalent, so they always come
// from the profile (force=false only fills them when empty, which they always
// are pre-S6.16).
void apply_endpoint_profile(LLMConfig& cfg, const EndpointProfile& p, bool force);

// File path resolver: <global_dir>/endpoints.json. Same dir family as the
// global mcp.json / prompts. (Design doc names %APPDATA%/Locus; the codebase
// settled on ~/.locus via global_paths::global_dir() in S5.M -- this follows
// that single source of truth.)
std::filesystem::path endpoints_path();

// Owns the global endpoints.json: load / save / mutate. Seeds six rows the
// first time the file is absent. All mutations are in-memory until save().
class EndpointProfileStore {
public:
    EndpointProfileStore() = default;

    // Load from `path` (defaults to endpoints_path()). If the file is absent
    // or unparseable, the store is seeded with the six builtins and `active`
    // is set to "LM Studio (local)". A successful load that yielded zero
    // profiles is also re-seeded. Returns true if the file existed and parsed.
    bool load(const std::filesystem::path& path = {});

    // Atomic temp+rename write to `path` (defaults to the loaded path, or
    // endpoints_path() when never loaded). Returns empty string on success,
    // an error message otherwise.
    std::string save(const std::filesystem::path& path = {}) const;

    // Replace the in-memory state with the six builtin seed rows + active
    // pointing at "LM Studio (local)". Does NOT write to disk.
    void seed_defaults();

    const std::vector<EndpointProfile>& list() const { return profiles_; }

    // Returns nullptr when no profile carries `name`.
    const EndpointProfile* find(const std::string& name) const;

    // Add a fresh profile. Refuses (returns false) when the name is empty or
    // already present. The added row is non-builtin.
    bool add(EndpointProfile p);

    // Replace the profile with matching `p.name`. Returns false when absent.
    // A builtin's `builtin` flag is preserved regardless of `p.builtin`.
    bool update(const EndpointProfile& p);

    // Remove a non-builtin profile by name. Builtins are never removed
    // (returns false). When the removed profile was active, `active` falls
    // back to the first profile.
    bool remove(const std::string& name);

    const std::string& active() const { return active_; }

    // Set the active profile. Refuses unknown names (returns false).
    bool set_active(const std::string& name);

    // Six builtin seed rows per the S6.16 Design table. Exposed for tests +
    // the migration path.
    static std::vector<EndpointProfile> builtin_profiles();

private:
    std::vector<EndpointProfile> profiles_;
    std::string                  active_;
    std::filesystem::path        loaded_path_;
};

} // namespace locus

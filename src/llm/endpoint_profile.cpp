#include "llm/endpoint_profile.h"

#include "core/global_paths.h"
#include "tools/shared.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace locus {

namespace fs = std::filesystem;
using nlohmann::json;

void apply_endpoint_profile(LLMConfig& cfg, const EndpointProfile& p, bool force)
{
    if (force) {
        cfg.base_url      = p.base_url;
        cfg.api_key       = p.api_key;
        cfg.extra_headers = p.extra_headers;
        cfg.model         = p.default_model;          // "" -> caller re-detects
        cfg.context_limit = p.default_context_limit;  // 0  -> caller re-detects
        if (!p.tool_format.empty())
            cfg.tool_format = tool_format_from_string(p.tool_format);
        return;
    }

    // Startup resolution: profile fills only unset/default sentinels.
    if (cfg.base_url.empty() || cfg.base_url == "http://127.0.0.1:1234")
        cfg.base_url = p.base_url;
    if (cfg.api_key.empty())
        cfg.api_key = p.api_key;
    if (cfg.extra_headers.empty())
        cfg.extra_headers = p.extra_headers;
    if (cfg.model.empty() && !p.default_model.empty())
        cfg.model = p.default_model;
    if (cfg.context_limit <= 0 && p.default_context_limit > 0)
        cfg.context_limit = p.default_context_limit;
    if (cfg.tool_format == ToolFormat::Auto && !p.tool_format.empty())
        cfg.tool_format = tool_format_from_string(p.tool_format);
}

fs::path endpoints_path()
{
    auto dir = global_paths::global_dir();
    if (dir.empty()) return {};
    return dir / "endpoints.json";
}

std::vector<EndpointProfile> EndpointProfileStore::builtin_profiles()
{
    std::vector<EndpointProfile> v;

    v.push_back({"LM Studio (local)", "http://127.0.0.1:1234", "",
                 "", 0, "auto", {}, true});

    v.push_back({"Ollama (local)", "http://127.0.0.1:11434/v1", "",
                 "", 0, "auto", {}, true});

    v.push_back({"NVIDIA Build (hosted)", "https://integrate.api.nvidia.com/v1", "",
                 "qwen/qwen3-coder-480b-a35b-instruct", 0, "openai", {}, true});

    v.push_back({"OpenAI (hosted)", "https://api.openai.com/v1", "",
                 "", 0, "openai", {}, true});

    v.push_back({"OpenRouter (hosted)", "https://openrouter.ai/api/v1", "",
                 "", 0, "auto", {}, true});

    v.push_back({"Claude via OpenAI-compat proxy", "http://127.0.0.1:8082/v1", "",
                 "", 0, "claude", {}, true});

    return v;
}

void EndpointProfileStore::seed_defaults()
{
    profiles_ = builtin_profiles();
    active_   = "LM Studio (local)";
}

const EndpointProfile* EndpointProfileStore::find(const std::string& name) const
{
    for (const auto& p : profiles_)
        if (p.name == name) return &p;
    return nullptr;
}

bool EndpointProfileStore::add(EndpointProfile p)
{
    if (p.name.empty()) return false;
    if (find(p.name)) return false;
    p.builtin = false;
    profiles_.push_back(std::move(p));
    return true;
}

bool EndpointProfileStore::update(const EndpointProfile& p)
{
    for (auto& existing : profiles_) {
        if (existing.name == p.name) {
            bool was_builtin = existing.builtin;
            existing = p;
            existing.builtin = was_builtin;  // builtin flag is not user-editable
            return true;
        }
    }
    return false;
}

bool EndpointProfileStore::remove(const std::string& name)
{
    auto it = std::find_if(profiles_.begin(), profiles_.end(),
                           [&](const EndpointProfile& p) { return p.name == name; });
    if (it == profiles_.end()) return false;
    if (it->builtin) return false;            // builtins are never removed
    profiles_.erase(it);
    if (active_ == name)
        active_ = profiles_.empty() ? std::string{} : profiles_.front().name;
    return true;
}

bool EndpointProfileStore::set_active(const std::string& name)
{
    if (!find(name)) return false;
    active_ = name;
    return true;
}

bool EndpointProfileStore::load(const fs::path& path_in)
{
    fs::path path = path_in.empty() ? endpoints_path() : path_in;
    loaded_path_ = path;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        seed_defaults();
        return false;
    }

    std::stringstream ss;
    ss << in.rdbuf();

    json doc;
    try {
        doc = json::parse(ss.str());
    } catch (const std::exception& e) {
        spdlog::warn("endpoints.json parse failed ({}); using seed defaults", e.what());
        seed_defaults();
        return false;
    }

    profiles_.clear();
    if (doc.contains("profiles") && doc["profiles"].is_array()) {
        for (const auto& j : doc["profiles"]) {
            EndpointProfile p;
            p.name     = j.value("name", std::string{});
            if (p.name.empty()) continue;  // empty-name rows are "hidden"
            p.base_url = j.value("base_url", std::string{});
            p.api_key  = j.value("api_key", std::string{});
            p.default_model = j.value("default_model", std::string{});
            p.default_context_limit = j.value("default_context_limit", 0);
            p.tool_format = j.value("tool_format", std::string{"auto"});
            p.builtin = j.value("builtin", false);
            if (j.contains("extra_headers") && j["extra_headers"].is_object()) {
                for (auto it = j["extra_headers"].begin();
                     it != j["extra_headers"].end(); ++it) {
                    if (it.value().is_string())
                        p.extra_headers[it.key()] = it.value().get<std::string>();
                }
            }
            profiles_.push_back(std::move(p));
        }
    }

    active_ = doc.value("active", std::string{});

    // Empty / all-hidden file -> re-seed so the user always has the builtins.
    if (profiles_.empty()) {
        seed_defaults();
        return false;
    }
    // Active points nowhere -> snap to first.
    if (active_.empty() || !find(active_))
        active_ = profiles_.front().name;

    return true;
}

std::string EndpointProfileStore::save(const fs::path& path_in) const
{
    fs::path path = path_in.empty()
                    ? (loaded_path_.empty() ? endpoints_path() : loaded_path_)
                    : path_in;
    if (path.empty())
        return "cannot resolve endpoints.json path (no global dir)";

    json doc;
    doc["version"] = 1;
    doc["active"]  = active_;
    json arr = json::array();
    for (const auto& p : profiles_) {
        json j;
        j["name"]                  = p.name;
        j["base_url"]              = p.base_url;
        j["api_key"]               = p.api_key;
        j["default_model"]         = p.default_model;
        j["default_context_limit"] = p.default_context_limit;
        j["tool_format"]           = p.tool_format;
        j["builtin"]               = p.builtin;
        if (!p.extra_headers.empty()) {
            json h = json::object();
            for (const auto& [k, v] : p.extra_headers) h[k] = v;
            j["extra_headers"] = std::move(h);
        }
        arr.push_back(std::move(j));
    }
    doc["profiles"] = std::move(arr);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    return tools::write_atomic(path, doc.dump(2));
}

} // namespace locus

#include "core/workspace_config_json.h"
#include "tools/tool.h"

namespace locus {

using json = nlohmann::json;

WorkspaceConfig workspace_config_from_json(const json& j)
{
    WorkspaceConfig cfg;

    if (j.contains("index")) {
        auto& idx = j["index"];
        if (idx.contains("exclude_patterns"))
            cfg.exclude_patterns = idx["exclude_patterns"].get<std::vector<std::string>>();
        if (idx.contains("max_file_size_kb"))
            cfg.max_file_size_kb = idx["max_file_size_kb"].get<int>();
        if (idx.contains("code_parsing_enabled"))
            cfg.code_parsing_enabled = idx["code_parsing_enabled"].get<bool>();
        if (idx.contains("respect_gitignore"))
            cfg.respect_gitignore = idx["respect_gitignore"].get<bool>();

        if (idx.contains("semantic_search")) {
            auto& ss = idx["semantic_search"];
            if (ss.contains("enabled"))
                cfg.semantic_search_enabled = ss["enabled"].get<bool>();
            if (ss.contains("model"))
                cfg.embedding_model = ss["model"].get<std::string>();
            // 'dimensions' is no longer a config setting - silently ignore
            // legacy entries so old config.json files don't error out.
            if (ss.contains("chunk_size_lines"))
                cfg.chunk_size_lines = ss["chunk_size_lines"].get<int>();
            if (ss.contains("chunk_overlap_lines"))
                cfg.chunk_overlap_lines = ss["chunk_overlap_lines"].get<int>();
            if (ss.contains("reranker_enabled"))
                cfg.reranker_enabled = ss["reranker_enabled"].get<bool>();
            if (ss.contains("reranker_model"))
                cfg.reranker_model = ss["reranker_model"].get<std::string>();
            if (ss.contains("reranker_top_k"))
                cfg.reranker_top_k = ss["reranker_top_k"].get<int>();
        }
    }

    if (j.contains("llm")) {
        auto& llm = j["llm"];
        if (llm.contains("endpoint"))
            cfg.llm_endpoint = llm["endpoint"].get<std::string>();
        if (llm.contains("model"))
            cfg.llm_model = llm["model"].get<std::string>();
        if (llm.contains("temperature"))
            cfg.llm_temperature = llm["temperature"].get<double>();
        if (llm.contains("context_limit"))
            cfg.llm_context_limit = llm["context_limit"].get<int>();
        if (llm.contains("max_tokens"))
            cfg.llm_max_tokens = llm["max_tokens"].get<int>();
        if (llm.contains("tool_format"))
            cfg.llm_tool_format = llm["tool_format"].get<std::string>();
        if (llm.contains("timeout_ms"))
            cfg.llm_timeout_ms = llm["timeout_ms"].get<int>();
        if (llm.contains("top_p"))
            cfg.llm_top_p = llm["top_p"].get<double>();
        if (llm.contains("top_k"))
            cfg.llm_top_k = llm["top_k"].get<int>();
        if (llm.contains("min_p"))
            cfg.llm_min_p = llm["min_p"].get<double>();
        if (llm.contains("repeat_penalty"))
            cfg.llm_repeat_penalty = llm["repeat_penalty"].get<double>();
        if (llm.contains("frequency_penalty"))
            cfg.llm_frequency_penalty = llm["frequency_penalty"].get<double>();
        if (llm.contains("presence_penalty"))
            cfg.llm_presence_penalty = llm["presence_penalty"].get<double>();
    }

    if (j.contains("agent")) {
        auto& ag = j["agent"];
        if (ag.contains("tool_manifest_warn_tokens"))
            cfg.tool_manifest_warn_tokens = ag["tool_manifest_warn_tokens"].get<int>();
        if (ag.contains("process_output_buffer_kb"))
            cfg.process_output_buffer_kb = ag["process_output_buffer_kb"].get<int>();
        if (ag.contains("notify_external_changes"))
            cfg.notify_external_changes = ag["notify_external_changes"].get<bool>();
    }

    if (j.contains("memory")) {
        auto& m = j["memory"];
        if (m.contains("enabled"))
            cfg.memory_enabled = m["enabled"].get<bool>();
        if (m.contains("in_context_budget_tokens"))
            cfg.memory_in_context_budget_tokens = m["in_context_budget_tokens"].get<int>();
        if (m.contains("max_entries"))
            cfg.memory_max_entries = m["max_entries"].get<int>();
        if (m.contains("search_response_max_tokens"))
            cfg.memory_search_response_max_tokens = m["search_response_max_tokens"].get<int>();
        if (m.contains("recency_half_life_days"))
            cfg.memory_recency_half_life_days = m["recency_half_life_days"].get<int>();
    }

    if (j.contains("git")) {
        auto& g = j["git"];
        if (g.contains("auto_commit"))
            cfg.git_auto_commit = g["auto_commit"].get<bool>();
        if (g.contains("commit_branch"))
            cfg.git_commit_branch = g["commit_branch"].get<std::string>();
        if (g.contains("commit_prefix"))
            cfg.git_commit_prefix = g["commit_prefix"].get<std::string>();
    }

    if (j.contains("capabilities") && j["capabilities"].is_object()) {
        auto& c = j["capabilities"];
        if (c.contains("background_processes"))
            cfg.capabilities.background_processes = c["background_processes"].get<bool>();
        if (c.contains("semantic_search"))
            cfg.capabilities.semantic_search      = c["semantic_search"].get<bool>();
        if (c.contains("code_aware_search"))
            cfg.capabilities.code_aware_search    = c["code_aware_search"].get<bool>();
        if (c.contains("memory_bank"))
            cfg.capabilities.memory_bank          = c["memory_bank"].get<bool>();
        if (c.contains("web_retrieval"))
            cfg.capabilities.web_retrieval        = c["web_retrieval"].get<bool>();
        // Capabilities are canonical when present -- propagate to the legacy
        // flags so the older subsystems (Workspace ctor, MemoryStore
        // construction) see the user's chosen value without a parallel UI.
        cfg.semantic_search_enabled = cfg.capabilities.semantic_search;
        cfg.memory_enabled          = cfg.capabilities.memory_bank;
    } else {
        // Migration path: derive the two mappable buckets from the existing
        // flags. The other three buckets keep their defaults.
        cfg.capabilities.semantic_search = cfg.semantic_search_enabled;
        cfg.capabilities.memory_bank     = cfg.memory_enabled;
    }

    if (j.contains("tool_approvals") && j["tool_approvals"].is_object()) {
        for (auto it = j["tool_approvals"].begin();
             it != j["tool_approvals"].end(); ++it) {
            if (it.value().is_string()) {
                cfg.tool_approval_policies[it.key()] =
                    policy_from_string(it.value().get<std::string>());
            }
        }
    }

    return cfg;
}

json workspace_config_to_json(const WorkspaceConfig& cfg)
{
    json approvals = json::object();
    for (const auto& [name, policy] : cfg.tool_approval_policies)
        approvals[name] = to_string(policy);

    return {
        {"index", {
            {"exclude_patterns", cfg.exclude_patterns},
            {"max_file_size_kb", cfg.max_file_size_kb},
            {"code_parsing_enabled", cfg.code_parsing_enabled},
            {"respect_gitignore", cfg.respect_gitignore},
            {"semantic_search", {
                {"enabled", cfg.semantic_search_enabled},
                {"model", cfg.embedding_model},
                {"chunk_size_lines", cfg.chunk_size_lines},
                {"chunk_overlap_lines", cfg.chunk_overlap_lines},
                {"reranker_enabled", cfg.reranker_enabled},
                {"reranker_model", cfg.reranker_model},
                {"reranker_top_k", cfg.reranker_top_k}
            }}
        }},
        {"llm", {
            {"endpoint", cfg.llm_endpoint},
            {"model", cfg.llm_model},
            {"temperature", cfg.llm_temperature},
            {"context_limit", cfg.llm_context_limit},
            {"max_tokens", cfg.llm_max_tokens},
            {"tool_format", cfg.llm_tool_format},
            {"timeout_ms", cfg.llm_timeout_ms},
            {"top_p",          cfg.llm_top_p},
            {"top_k",          cfg.llm_top_k},
            {"min_p",          cfg.llm_min_p},
            {"repeat_penalty", cfg.llm_repeat_penalty},
            {"frequency_penalty", cfg.llm_frequency_penalty},
            {"presence_penalty",  cfg.llm_presence_penalty}
        }},
        {"agent", {
            {"tool_manifest_warn_tokens", cfg.tool_manifest_warn_tokens},
            {"process_output_buffer_kb",  cfg.process_output_buffer_kb},
            {"notify_external_changes",   cfg.notify_external_changes}
        }},
        {"memory", {
            {"enabled",                    cfg.memory_enabled},
            {"in_context_budget_tokens",   cfg.memory_in_context_budget_tokens},
            {"max_entries",                cfg.memory_max_entries},
            {"search_response_max_tokens", cfg.memory_search_response_max_tokens},
            {"recency_half_life_days",     cfg.memory_recency_half_life_days}
        }},
        {"git", {
            {"auto_commit",   cfg.git_auto_commit},
            {"commit_branch", cfg.git_commit_branch},
            {"commit_prefix", cfg.git_commit_prefix}
        }},
        {"capabilities", {
            {"background_processes", cfg.capabilities.background_processes},
            {"semantic_search",      cfg.capabilities.semantic_search},
            {"code_aware_search",    cfg.capabilities.code_aware_search},
            {"memory_bank",          cfg.capabilities.memory_bank},
            {"web_retrieval",        cfg.capabilities.web_retrieval}
        }},
        {"tool_approvals", approvals}
    };
}

} // namespace locus

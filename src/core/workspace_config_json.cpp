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
            cfg.index.exclude_patterns = idx["exclude_patterns"].get<std::vector<std::string>>();
        if (idx.contains("max_file_size_kb"))
            cfg.index.max_file_size_kb = idx["max_file_size_kb"].get<int>();
        if (idx.contains("code_parsing_enabled"))
            cfg.index.code_parsing_enabled = idx["code_parsing_enabled"].get<bool>();
        if (idx.contains("respect_gitignore"))
            cfg.index.respect_gitignore = idx["respect_gitignore"].get<bool>();

        if (idx.contains("semantic_search")) {
            auto& ss = idx["semantic_search"];
            if (ss.contains("enabled"))
                cfg.index.semantic_search_enabled = ss["enabled"].get<bool>();
            if (ss.contains("model"))
                cfg.index.embedding_model = ss["model"].get<std::string>();
            // 'dimensions' is no longer a config setting - silently ignore
            // legacy entries so old config.json files don't error out.
            if (ss.contains("chunk_size_lines"))
                cfg.index.chunk_size_lines = ss["chunk_size_lines"].get<int>();
            if (ss.contains("chunk_overlap_lines"))
                cfg.index.chunk_overlap_lines = ss["chunk_overlap_lines"].get<int>();
            if (ss.contains("reranker_enabled"))
                cfg.index.reranker_enabled = ss["reranker_enabled"].get<bool>();
            if (ss.contains("reranker_model"))
                cfg.index.reranker_model = ss["reranker_model"].get<std::string>();
            if (ss.contains("reranker_top_k"))
                cfg.index.reranker_top_k = ss["reranker_top_k"].get<int>();
        }
    }

    if (j.contains("llm")) {
        auto& llm = j["llm"];
        if (llm.contains("endpoint"))
            cfg.llm.endpoint = llm["endpoint"].get<std::string>();
        if (llm.contains("model"))
            cfg.llm.model = llm["model"].get<std::string>();
        if (llm.contains("temperature"))
            cfg.llm.temperature = llm["temperature"].get<double>();
        if (llm.contains("context_limit"))
            cfg.llm.context_limit = llm["context_limit"].get<int>();
        if (llm.contains("max_tokens"))
            cfg.llm.max_tokens = llm["max_tokens"].get<int>();
        if (llm.contains("tool_format"))
            cfg.llm.tool_format = llm["tool_format"].get<std::string>();
        if (llm.contains("timeout_ms"))
            cfg.llm.timeout_ms = llm["timeout_ms"].get<int>();
        if (llm.contains("top_p"))
            cfg.llm.top_p = llm["top_p"].get<double>();
        if (llm.contains("top_k"))
            cfg.llm.top_k = llm["top_k"].get<int>();
        if (llm.contains("min_p"))
            cfg.llm.min_p = llm["min_p"].get<double>();
        if (llm.contains("repeat_penalty"))
            cfg.llm.repeat_penalty = llm["repeat_penalty"].get<double>();
        if (llm.contains("frequency_penalty"))
            cfg.llm.frequency_penalty = llm["frequency_penalty"].get<double>();
        if (llm.contains("presence_penalty"))
            cfg.llm.presence_penalty = llm["presence_penalty"].get<double>();
        // S6.10 Task D -- grammar-constrained decoding for tool calls.
        if (llm.contains("grammar_mode"))
            cfg.llm.grammar_mode = llm["grammar_mode"].get<std::string>();
        // S6.10 Task F -- auto-detect model and apply preset
        if (llm.contains("auto_detect_preset"))
            cfg.llm.auto_detect_preset = llm["auto_detect_preset"].get<bool>();
        if (llm.contains("preset_name"))
            cfg.llm.preset_name = llm["preset_name"].get<std::string>();
    }

    if (j.contains("agent")) {
        auto& ag = j["agent"];
        if (ag.contains("tool_manifest_warn_tokens"))
            cfg.agent.tool_manifest_warn_tokens = ag["tool_manifest_warn_tokens"].get<int>();
        if (ag.contains("process_output_buffer_kb"))
            cfg.agent.process_output_buffer_kb = ag["process_output_buffer_kb"].get<int>();
        if (ag.contains("tool_max_runtime_s"))
            cfg.agent.tool_max_runtime_s = ag["tool_max_runtime_s"].get<int>();
        if (ag.contains("max_rounds_per_message"))
            cfg.agent.max_rounds_per_message = ag["max_rounds_per_message"].get<int>();
        if (ag.contains("run_command_truncate_lines"))
            cfg.agent.run_command_truncate_lines = ag["run_command_truncate_lines"].get<int>();
        if (ag.contains("dump_on_run_command_hang"))
            cfg.agent.dump_on_run_command_hang = ag["dump_on_run_command_hang"].get<bool>();
        if (ag.contains("notify_external_changes"))
            cfg.agent.notify_external_changes = ag["notify_external_changes"].get<bool>();
        if (ag.contains("require_read_before_edit"))
            cfg.agent.require_read_before_edit = ag["require_read_before_edit"].get<bool>();
        // S6.10 Task G -- anti-truncation detector
        if (ag.contains("detect_write_truncation"))
            cfg.agent.detect_write_truncation = ag["detect_write_truncation"].get<bool>();
        // S6.10 Task B -- quality monitor
        if (ag.contains("quality_monitor_enabled"))
            cfg.agent.quality_monitor_enabled = ag["quality_monitor_enabled"].get<bool>();
        // S6.10 Task C -- strip past-turn reasoning_content from LLM payload
        if (ag.contains("strip_past_thinking"))
            cfg.agent.strip_past_thinking = ag["strip_past_thinking"].get<bool>();
        // S6.11 -- lazy tool manifest
        if (ag.contains("lazy_tool_manifest"))
            cfg.agent.lazy_tool_manifest = ag["lazy_tool_manifest"].get<bool>();
        // S6.12 -- system-prompt profile
        if (ag.contains("system_prompt_profile"))
            cfg.agent.system_prompt_profile = ag["system_prompt_profile"].get<std::string>();
        // S6.17 Task H -- prompt-cost preset (overrides the two flags above
        // at the apply step below).
        if (ag.contains("prompt_cost") && ag["prompt_cost"].is_string())
            cfg.agent.prompt_cost = ag["prompt_cost"].get<std::string>();
        // S6.13 -- reasoning watchdog
        if (ag.contains("reasoning_max_seconds"))
            cfg.agent.reasoning_max_seconds = ag["reasoning_max_seconds"].get<int>();
        if (ag.contains("reasoning_max_chars"))
            cfg.agent.reasoning_max_chars = ag["reasoning_max_chars"].get<int>();
        if (ag.contains("reasoning_max_rounds_silent"))
            cfg.agent.reasoning_max_rounds_silent = ag["reasoning_max_rounds_silent"].get<int>();
        if (ag.contains("reasoning_auto_nudge"))
            cfg.agent.reasoning_auto_nudge = ag["reasoning_auto_nudge"].get<bool>();
    }

    if (j.contains("memory")) {
        auto& m = j["memory"];
        if (m.contains("enabled"))
            cfg.memory.enabled = m["enabled"].get<bool>();
        if (m.contains("in_context_budget_tokens"))
            cfg.memory.in_context_budget_tokens = m["in_context_budget_tokens"].get<int>();
        if (m.contains("max_entries"))
            cfg.memory.max_entries = m["max_entries"].get<int>();
        if (m.contains("search_response_max_tokens"))
            cfg.memory.search_response_max_tokens = m["search_response_max_tokens"].get<int>();
        if (m.contains("recency_half_life_days"))
            cfg.memory.recency_half_life_days = m["recency_half_life_days"].get<int>();
    }

    if (j.contains("git")) {
        auto& g = j["git"];
        if (g.contains("auto_commit"))
            cfg.git.auto_commit = g["auto_commit"].get<bool>();
        if (g.contains("commit_branch"))
            cfg.git.commit_branch = g["commit_branch"].get<std::string>();
        if (g.contains("commit_prefix"))
            cfg.git.commit_prefix = g["commit_prefix"].get<std::string>();
    }

    if (j.contains("chat")) {
        auto& c = j["chat"];
        if (c.contains("show_diffs"))
            cfg.chat.show_diffs = c["show_diffs"].get<bool>();
        if (c.contains("diff_max_lines"))
            cfg.chat.diff_max_lines = c["diff_max_lines"].get<int>();
        if (c.contains("diff_context_lines"))
            cfg.chat.diff_context_lines = c["diff_context_lines"].get<int>();
        if (c.contains("diff_collapse_threshold"))
            cfg.chat.diff_collapse_threshold = c["diff_collapse_threshold"].get<int>();
    }

    if (j.contains("compaction")) {
        auto& c = j["compaction"];
        if (c.contains("reserve_tokens"))
            cfg.compaction.reserve_tokens = c["reserve_tokens"].get<int>();
        if (c.contains("auto_enabled"))
            cfg.compaction.auto_enabled = c["auto_enabled"].get<bool>();
        if (c.contains("warn_threshold"))
            cfg.compaction.warn_threshold = c["warn_threshold"].get<double>();
        if (c.contains("auto_threshold"))
            cfg.compaction.auto_threshold = c["auto_threshold"].get<double>();
        if (c.contains("strip_threshold_tokens"))
            cfg.compaction.strip_threshold_tokens = c["strip_threshold_tokens"].get<int>();
        if (c.contains("older_than_turns"))
            cfg.compaction.older_than_turns = c["older_than_turns"].get<int>();
        if (c.contains("keep_recent_turns"))
            cfg.compaction.keep_recent_turns = c["keep_recent_turns"].get<int>();
        if (c.contains("summary_max_tokens"))
            cfg.compaction.summary_max_tokens = c["summary_max_tokens"].get<int>();
        if (c.contains("archive_keep_count"))
            cfg.compaction.archive_keep_count = c["archive_keep_count"].get<int>();
        if (c.contains("preserve_short_user_msgs_max_tokens"))
            cfg.compaction.preserve_short_user_msgs_max_tokens =
                c["preserve_short_user_msgs_max_tokens"].get<int>();
        if (c.contains("preserve_short_tool_calls_max_tokens"))
            cfg.compaction.preserve_short_tool_calls_max_tokens =
                c["preserve_short_tool_calls_max_tokens"].get<int>();
        if (c.contains("custom_summary_instructions"))
            cfg.compaction.custom_summary_instructions =
                c["custom_summary_instructions"].get<std::string>();
        if (c.contains("layer_drop_redundant_tool_results"))
            cfg.compaction.layer_drop_redundant_tool_results =
                c["layer_drop_redundant_tool_results"].get<bool>();
        if (c.contains("layer_strip_large_tool_bodies"))
            cfg.compaction.layer_strip_large_tool_bodies =
                c["layer_strip_large_tool_bodies"].get<bool>();
        if (c.contains("layer_drop_old_reasoning"))
            cfg.compaction.layer_drop_old_reasoning =
                c["layer_drop_old_reasoning"].get<bool>();
        if (c.contains("layer_drop_oldest_turns"))
            cfg.compaction.layer_drop_oldest_turns =
                c["layer_drop_oldest_turns"].get<bool>();
        if (c.contains("layer_llm_summary"))
            cfg.compaction.layer_llm_summary =
                c["layer_llm_summary"].get<bool>();
        // S6.17 Task B.3 -- preset selector.
        if (c.contains("aggressiveness") && c["aggressiveness"].is_string())
            cfg.compaction.aggressiveness = c["aggressiveness"].get<std::string>();
        // S6.17 Task B.1 -- count-based heuristic knobs.
        if (c.contains("count_heuristic_window"))
            cfg.compaction.count_heuristic_window =
                c["count_heuristic_window"].get<int>();
        if (c.contains("count_heuristic_threshold"))
            cfg.compaction.count_heuristic_threshold =
                c["count_heuristic_threshold"].get<int>();
    }

    if (j.contains("ui")) {
        auto& u = j["ui"];
        if (u.contains("show_per_message_tokens"))
            cfg.chat.show_per_message_tokens = u["show_per_message_tokens"].get<bool>();
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
        cfg.index.semantic_search_enabled = cfg.capabilities.semantic_search;
        cfg.memory.enabled                = cfg.capabilities.memory_bank;
    } else {
        // Migration path: derive the two mappable buckets from the existing
        // flags. The other three buckets keep their defaults.
        cfg.capabilities.semantic_search = cfg.index.semantic_search_enabled;
        cfg.capabilities.memory_bank     = cfg.memory.enabled;
    }

    if (j.contains("sessions") && j["sessions"].is_object()) {
        auto& s = j["sessions"];
        if (s.contains("auto_cleanup_enabled"))
            cfg.sessions.auto_cleanup_enabled = s["auto_cleanup_enabled"].get<bool>();
        if (s.contains("keep_last_count") && s["keep_last_count"].is_number_integer())
            cfg.sessions.keep_last_count = s["keep_last_count"].get<int>();
        if (s.contains("delete_after_days") && s["delete_after_days"].is_number_integer())
            cfg.sessions.delete_after_days = s["delete_after_days"].get<int>();
        if (s.contains("restore_last"))
            cfg.sessions.restore_last = s["restore_last"].get<bool>();
        if (s.contains("persist_activity"))
            cfg.sessions.persist_activity = s["persist_activity"].get<bool>();
    }

    if (j.contains("notifications") && j["notifications"].is_object()) {
        auto& n = j["notifications"];
        if (n.contains("sound_on_tool_approval"))
            cfg.notifications.sound_on_tool_approval =
                n["sound_on_tool_approval"].get<bool>();
        if (n.contains("sound_on_ask_user"))
            cfg.notifications.sound_on_ask_user =
                n["sound_on_ask_user"].get<bool>();
        if (n.contains("sound_on_turn_complete"))
            cfg.notifications.sound_on_turn_complete =
                n["sound_on_turn_complete"].get<bool>();
        if (n.contains("sound_on_compaction"))
            cfg.notifications.sound_on_compaction =
                n["sound_on_compaction"].get<bool>();
        if (n.contains("only_when_unfocused"))
            cfg.notifications.only_when_unfocused =
                n["only_when_unfocused"].get<bool>();
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

    // S6.17 Task H -- resolve the prompt_cost preset (if set) into the
    // lazy_tool_manifest + system_prompt_profile fields so the rest of the
    // codebase sees the resolved values without having to know the preset.
    prompt_cost_apply(cfg);

    return cfg;
}

json workspace_config_to_json(const WorkspaceConfig& cfg)
{
    json approvals = json::object();
    for (const auto& [name, policy] : cfg.tool_approval_policies)
        approvals[name] = to_string(policy);

    return {
        {"index", {
            {"exclude_patterns", cfg.index.exclude_patterns},
            {"max_file_size_kb", cfg.index.max_file_size_kb},
            {"code_parsing_enabled", cfg.index.code_parsing_enabled},
            {"respect_gitignore", cfg.index.respect_gitignore},
            {"semantic_search", {
                {"enabled", cfg.index.semantic_search_enabled},
                {"model", cfg.index.embedding_model},
                {"chunk_size_lines", cfg.index.chunk_size_lines},
                {"chunk_overlap_lines", cfg.index.chunk_overlap_lines},
                {"reranker_enabled", cfg.index.reranker_enabled},
                {"reranker_model", cfg.index.reranker_model},
                {"reranker_top_k", cfg.index.reranker_top_k}
            }}
        }},
        {"llm", {
            {"endpoint", cfg.llm.endpoint},
            {"model", cfg.llm.model},
            {"temperature", cfg.llm.temperature},
            {"context_limit", cfg.llm.context_limit},
            {"max_tokens", cfg.llm.max_tokens},
            {"tool_format", cfg.llm.tool_format},
            {"timeout_ms", cfg.llm.timeout_ms},
            {"top_p",          cfg.llm.top_p},
            {"top_k",          cfg.llm.top_k},
            {"min_p",          cfg.llm.min_p},
            {"repeat_penalty", cfg.llm.repeat_penalty},
            {"frequency_penalty", cfg.llm.frequency_penalty},
            {"presence_penalty",  cfg.llm.presence_penalty},
            {"grammar_mode",      cfg.llm.grammar_mode},
            {"auto_detect_preset", cfg.llm.auto_detect_preset},
            {"preset_name",       cfg.llm.preset_name}
        }},
        {"agent", {
            {"tool_manifest_warn_tokens", cfg.agent.tool_manifest_warn_tokens},
            {"process_output_buffer_kb",  cfg.agent.process_output_buffer_kb},
            {"tool_max_runtime_s",        cfg.agent.tool_max_runtime_s},
            {"max_rounds_per_message",    cfg.agent.max_rounds_per_message},
            {"run_command_truncate_lines", cfg.agent.run_command_truncate_lines},
            {"dump_on_run_command_hang",  cfg.agent.dump_on_run_command_hang},
            {"notify_external_changes",   cfg.agent.notify_external_changes},
            {"require_read_before_edit",  cfg.agent.require_read_before_edit},
            {"detect_write_truncation",   cfg.agent.detect_write_truncation},
            {"quality_monitor_enabled",   cfg.agent.quality_monitor_enabled},
            {"strip_past_thinking",       cfg.agent.strip_past_thinking},
            {"lazy_tool_manifest",        cfg.agent.lazy_tool_manifest},
            {"system_prompt_profile",     cfg.agent.system_prompt_profile},
            {"prompt_cost",               cfg.agent.prompt_cost},
            {"reasoning_max_seconds",       cfg.agent.reasoning_max_seconds},
            {"reasoning_max_chars",         cfg.agent.reasoning_max_chars},
            {"reasoning_max_rounds_silent", cfg.agent.reasoning_max_rounds_silent},
            {"reasoning_auto_nudge",        cfg.agent.reasoning_auto_nudge}
        }},
        {"memory", {
            {"enabled",                    cfg.memory.enabled},
            {"in_context_budget_tokens",   cfg.memory.in_context_budget_tokens},
            {"max_entries",                cfg.memory.max_entries},
            {"search_response_max_tokens", cfg.memory.search_response_max_tokens},
            {"recency_half_life_days",     cfg.memory.recency_half_life_days}
        }},
        {"git", {
            {"auto_commit",   cfg.git.auto_commit},
            {"commit_branch", cfg.git.commit_branch},
            {"commit_prefix", cfg.git.commit_prefix}
        }},
        {"chat", {
            {"show_diffs",              cfg.chat.show_diffs},
            {"diff_max_lines",          cfg.chat.diff_max_lines},
            {"diff_context_lines",      cfg.chat.diff_context_lines},
            {"diff_collapse_threshold", cfg.chat.diff_collapse_threshold}
        }},
        {"capabilities", {
            {"background_processes", cfg.capabilities.background_processes},
            {"semantic_search",      cfg.capabilities.semantic_search},
            {"code_aware_search",    cfg.capabilities.code_aware_search},
            {"memory_bank",          cfg.capabilities.memory_bank},
            {"web_retrieval",        cfg.capabilities.web_retrieval}
        }},
        {"tool_approvals", approvals},
        {"compaction", {
            {"reserve_tokens", cfg.compaction.reserve_tokens},
            {"auto_enabled",                       cfg.compaction.auto_enabled},
            {"warn_threshold",                     cfg.compaction.warn_threshold},
            {"auto_threshold",                     cfg.compaction.auto_threshold},
            {"strip_threshold_tokens",             cfg.compaction.strip_threshold_tokens},
            {"older_than_turns",                   cfg.compaction.older_than_turns},
            {"keep_recent_turns",                  cfg.compaction.keep_recent_turns},
            {"summary_max_tokens",                 cfg.compaction.summary_max_tokens},
            {"archive_keep_count",                 cfg.compaction.archive_keep_count},
            {"preserve_short_user_msgs_max_tokens",  cfg.compaction.preserve_short_user_msgs_max_tokens},
            {"preserve_short_tool_calls_max_tokens", cfg.compaction.preserve_short_tool_calls_max_tokens},
            {"custom_summary_instructions",        cfg.compaction.custom_summary_instructions},
            {"layer_drop_redundant_tool_results",  cfg.compaction.layer_drop_redundant_tool_results},
            {"layer_strip_large_tool_bodies",      cfg.compaction.layer_strip_large_tool_bodies},
            {"layer_drop_old_reasoning",           cfg.compaction.layer_drop_old_reasoning},
            {"layer_drop_oldest_turns",            cfg.compaction.layer_drop_oldest_turns},
            {"layer_llm_summary",                  cfg.compaction.layer_llm_summary},
            {"aggressiveness",                     cfg.compaction.aggressiveness},
            {"count_heuristic_window",             cfg.compaction.count_heuristic_window},
            {"count_heuristic_threshold",          cfg.compaction.count_heuristic_threshold}
        }},
        {"ui", {
            {"show_per_message_tokens", cfg.chat.show_per_message_tokens}
        }},
        {"sessions", {
            {"auto_cleanup_enabled", cfg.sessions.auto_cleanup_enabled},
            {"keep_last_count",      cfg.sessions.keep_last_count},
            {"delete_after_days",    cfg.sessions.delete_after_days},
            {"restore_last",         cfg.sessions.restore_last},
            {"persist_activity",     cfg.sessions.persist_activity}
        }},
        {"notifications", {
            {"sound_on_tool_approval", cfg.notifications.sound_on_tool_approval},
            {"sound_on_ask_user",      cfg.notifications.sound_on_ask_user},
            {"sound_on_turn_complete", cfg.notifications.sound_on_turn_complete},
            {"sound_on_compaction",    cfg.notifications.sound_on_compaction},
            {"only_when_unfocused",    cfg.notifications.only_when_unfocused}
        }}
    };
}

// S6.17 Task H -- prompt_cost preset helpers.

PromptCostFlags prompt_cost_to_flags(const std::string& preset,
                                     int context_limit,
                                     bool fallback_lazy,
                                     const std::string& fallback_profile)
{
    // Resolve "default" / empty against the context window. The spec says
    // <=16k -> balanced, >64k -> verbose, else honour the individual flags.
    std::string effective = preset;
    if (effective == "default") {
        if (context_limit > 0 && context_limit <= 16000)       effective = "balanced";
        else if (context_limit > 64000)                         effective = "verbose";
        else                                                    effective.clear();
    }

    if (effective == "minimal")  return {true,  "minimal"};
    if (effective == "balanced") return {true,  "compact"};
    if (effective == "verbose")  return {false, "full"};
    // Empty / unknown -> fall through to caller's manual flags.
    return {fallback_lazy, fallback_profile};
}

void prompt_cost_apply(WorkspaceConfig& cfg)
{
    if (cfg.agent.prompt_cost.empty()) return;
    auto resolved = prompt_cost_to_flags(
        cfg.agent.prompt_cost,
        cfg.llm.context_limit,
        cfg.agent.lazy_tool_manifest,
        cfg.agent.system_prompt_profile);
    cfg.agent.lazy_tool_manifest   = resolved.lazy_tool_manifest;
    cfg.agent.system_prompt_profile = resolved.system_prompt_profile;
}

} // namespace locus

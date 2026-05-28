#include "compaction_pipeline.h"
#include "compaction_prompts.h"

#include "llm/token_counter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>

namespace locus {

AggressivenessLayers layers_for_aggressiveness(const std::string& preset)
{
    // Empty == "balanced" for backward compat with pre-S6.17 configs.
    const std::string& p = preset.empty() ? std::string("balanced") : preset;
    if (p == "gentle")
        return {true, true, false, false, false};
    if (p == "aggressive")
        return {true, true, true, true, true};
    // balanced (default) + any unknown string -> balanced
    return {true, true, true, false, true};
}

CompactionLayerSelection selection_from_config(const WorkspaceConfig::Compaction& c)
{
    CompactionLayerSelection sel;
    // S6.17 Task B.3 -- preset selects the layer matrix unless the user
    // explicitly opted into the per-layer override mode ("custom").
    if (c.aggressiveness != "custom") {
        auto m = layers_for_aggressiveness(c.aggressiveness);
        sel.drop_redundant_tool_results = m.drop_redundant_tool_results;
        sel.strip_large_tool_bodies     = m.strip_large_tool_bodies;
        sel.drop_old_reasoning          = m.drop_old_reasoning;
        sel.drop_oldest_turns           = m.drop_oldest_turns;
        sel.llm_summary                 = m.llm_summary;
    } else {
        sel.drop_redundant_tool_results = c.layer_drop_redundant_tool_results;
        sel.strip_large_tool_bodies     = c.layer_strip_large_tool_bodies;
        sel.drop_old_reasoning          = c.layer_drop_old_reasoning;
        sel.drop_oldest_turns           = c.layer_drop_oldest_turns;
        sel.llm_summary                 = c.layer_llm_summary;
    }
    sel.strip_threshold_tokens                = c.strip_threshold_tokens;
    sel.older_than_turns                      = c.older_than_turns;
    sel.keep_recent_turns                     = c.keep_recent_turns;
    sel.preserve_short_user_msgs_max_tokens   = c.preserve_short_user_msgs_max_tokens;
    sel.preserve_short_tool_calls_max_tokens  = c.preserve_short_tool_calls_max_tokens;
    sel.summary_max_tokens                    = c.summary_max_tokens;
    sel.count_heuristic_window                = c.count_heuristic_window;
    sel.count_heuristic_threshold             = c.count_heuristic_threshold;
    sel.custom_summary_instructions           = c.custom_summary_instructions;
    return sel;
}

namespace {

// ---- Substitution helper (mirrors prompt_templates' shape) ----------------

std::string substitute_template(const std::string& tpl,
                                const std::string& conversation,
                                const std::string& custom_instructions)
{
    std::string out;
    out.reserve(tpl.size() + conversation.size() + custom_instructions.size());
    for (std::size_t i = 0; i < tpl.size();) {
        if (tpl[i] == '{' && i + 1 < tpl.size()) {
            // {conversation} / {custom_instructions}
            if (tpl.compare(i, 14, "{conversation}") == 0) {
                out += conversation;
                i += 14;
                continue;
            }
            if (tpl.compare(i, 21, "{custom_instructions}") == 0) {
                if (!custom_instructions.empty()) {
                    out += "Additional instructions from the user:\n";
                    out += custom_instructions;
                    out += "\n";
                }
                i += 21;
                continue;
            }
        }
        out += tpl[i++];
    }
    return out;
}

// ---- Locate the first user message ---------------------------------------

std::size_t find_first_user(const std::vector<ChatMessage>& msgs)
{
    for (std::size_t i = 0; i < msgs.size(); ++i)
        if (msgs[i].role == MessageRole::user)
            return i;
    return msgs.size();
}

// S6.18 Task B.1 -- find the most recent assistant message whose `content`
// is non-empty AND that has no `tool_calls`. That is the model's last
// "free-text" answer; pinning it makes the post-compaction history readable
// to the model ("here's the last thing I told the user") rather than ending
// with a stripped tool result. Returns msgs.size() when no such message
// exists (e.g. a session that has only made tool calls so far).
std::size_t find_last_assistant_text(const std::vector<ChatMessage>& msgs)
{
    for (std::size_t i = msgs.size(); i-- > 0; ) {
        const auto& m = msgs[i];
        if (m.role != MessageRole::assistant) continue;
        if (!m.tool_calls.empty()) continue;
        if (m.content.empty()) continue;
        return i;
    }
    return msgs.size();
}

// Walk turn boundaries from index `from` to compute the cutoff index of
// "the most recent `keep_recent` turn pairs". A turn is a contiguous block
// starting with a user message (or assistant-without-tool-calls when the
// model branches; rare in practice).
std::size_t keep_recent_cutoff(const std::vector<ChatMessage>& msgs,
                               int keep_recent)
{
    if (keep_recent <= 0) return msgs.size();

    // Collect user-message indices in order.
    std::vector<std::size_t> user_idxs;
    user_idxs.reserve(msgs.size() / 2 + 1);
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].role == MessageRole::user)
            user_idxs.push_back(i);
    }
    if (static_cast<int>(user_idxs.size()) <= keep_recent)
        return user_idxs.empty() ? msgs.size() : user_idxs.front();
    return user_idxs[user_idxs.size() - keep_recent];
}

// ---- Layer 3 helper: strip <think>...</think> blobs -----------------------

int strip_think_blocks(std::string& content)
{
    int removed = 0;
    std::size_t pos = 0;
    while (pos < content.size()) {
        auto open = content.find("<think>", pos);
        if (open == std::string::npos) break;
        auto close = content.find("</think>", open + 7);
        if (close == std::string::npos) break;
        std::size_t end = close + 8;
        // Eat trailing newline if present so we don't leave a blank line.
        if (end < content.size() && content[end] == '\n') ++end;
        content.erase(open, end - open);
        ++removed;
        pos = open;
    }
    return removed;
}

// ---- Build (tool_name + args) -> [tool_result_idx ...] map ----------------
//
// Tool-result messages carry a `tool_call_id`; the assistant message that
// preceded them owns the `(name, arguments)` for that id. We walk forward and
// keep a rolling map id -> (name, args).

struct ToolCallKey {
    std::string name;
    std::string args;
    bool operator==(const ToolCallKey& other) const {
        return name == other.name && args == other.args;
    }
};
struct ToolCallKeyHash {
    std::size_t operator()(const ToolCallKey& k) const {
        return std::hash<std::string>{}(k.name) ^
               (std::hash<std::string>{}(k.args) << 1);
    }
};

// Normalise arguments JSON by stripping whitespace -- two semantically
// identical calls that differ only in formatting still de-dupe.
std::string normalise_args(const std::string& args)
{
    std::string out;
    out.reserve(args.size());
    bool in_string = false;
    bool escaped   = false;
    for (char c : args) {
        if (in_string) {
            out += c;
            if (escaped) { escaped = false; continue; }
            if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; out += c; continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        out += c;
    }
    return out;
}

// ---- Extract "previous-attempt notes" from a dropped span -----------------
//
// During a long build-fix loop a small local model often hits the same
// compile error several rounds in a row, then a compaction wipes the trail
// and it re-tries one of the same wrong approaches. The deterministic Layer
// 6 summary is generated by the LLM, which can lose specifics. We append a
// short, structured "Notes from compacted turns:" block listing each
// (tool_call, failure) pair we can see in the dropped span so the next
// turn's prompt still carries "you already tried X and it produced Y" as
// data, not paraphrase.
//
// A tool-result message is considered a failure when its content
//   - starts with "Error:" (the convention every tool in src/tools/ uses
//     via tools::error_result(), and the dispatcher's "[disabled: ...]" /
//     "[tool wall-clock guardrail ...]" / "[tool '...' failed ...]" stubs
//     all match the bracket-leading pattern), OR
//   - contains "[exit code: " followed by a non-zero digit (run_command).
//
// Returns an empty string when nothing failed -- caller appends only when
// non-empty so the summary message format stays tight.

bool looks_like_failure(const std::string& content)
{
    if (content.empty()) return false;
    if (content.rfind("Error:", 0) == 0) return true;
    if (content.rfind("[disabled:", 0) == 0) return true;
    if (content.rfind("[tool ", 0) == 0) return true;
    auto pos = content.find("[exit code: ");
    if (pos != std::string::npos) {
        // Look at the first digit after the prefix; "0" -> success.
        std::size_t i = pos + 12;
        while (i < content.size() && content[i] == ' ') ++i;
        if (i < content.size() && content[i] >= '1' && content[i] <= '9')
            return true;
    }
    return false;
}

std::string brief_args(const std::string& tool_name, const std::string& args)
{
    try {
        auto j = nlohmann::json::parse(args);
        if (tool_name == "run_command" || tool_name == "run_command_bg") {
            if (j.contains("command") && j["command"].is_string())
                return j["command"].get<std::string>();
        }
        if (tool_name == "edit_file" || tool_name == "write_file" ||
            tool_name == "read_file" || tool_name == "delete_file") {
            // edit_file uses `file_path` (Claude Code convention); others
            // still use `path`. Try canonical first, fall back to legacy.
            if (j.contains("file_path") && j["file_path"].is_string())
                return j["file_path"].get<std::string>();
            if (j.contains("path") && j["path"].is_string())
                return j["path"].get<std::string>();
        }
        // Legacy unified `search` + post-S6.17 per-mode search_* tools all
        // accept a `query` arg, except search_symbols which uses `name`.
        if (tool_name == "search"
            || tool_name == "search_text"
            || tool_name == "search_regex"
            || tool_name == "search_semantic"
            || tool_name == "search_ast") {
            if (j.contains("query") && j["query"].is_string())
                return j["query"].get<std::string>();
        }
        if (tool_name == "search_symbols") {
            if (j.contains("name") && j["name"].is_string())
                return j["name"].get<std::string>();
        }
    } catch (...) {
        // Malformed args -- fall through to raw-string preview.
    }
    return args;
}

std::string first_line(const std::string& s, std::size_t cap = 200)
{
    auto nl = s.find('\n');
    std::string head = (nl == std::string::npos) ? s : s.substr(0, nl);
    // Strip a single leading "[exit code: N]" line so the second line (which
    // usually carries the actual diagnostic) wins on the body field.
    if (head.rfind("[exit code:", 0) == 0 && nl != std::string::npos) {
        std::size_t next_nl = s.find('\n', nl + 1);
        head = (next_nl == std::string::npos)
                   ? s.substr(nl + 1)
                   : s.substr(nl + 1, next_nl - nl - 1);
    }
    if (head.size() > cap) head = head.substr(0, cap) + "...";
    return head;
}

std::string extract_failure_notes(const std::vector<ChatMessage>& msgs,
                                  std::size_t begin, std::size_t end)
{
    // Map id -> (name, args) from assistant tool_calls in the span.
    std::unordered_map<std::string, std::pair<std::string,std::string>> by_id;
    for (std::size_t i = begin; i < end && i < msgs.size(); ++i) {
        if (msgs[i].role == MessageRole::assistant) {
            for (auto& tc : msgs[i].tool_calls)
                by_id[tc.id] = {tc.name, tc.arguments};
        }
    }

    std::ostringstream notes;
    std::size_t shown = 0;
    constexpr std::size_t k_max_notes = 6;       // keep the slot bounded
    constexpr std::size_t k_arg_cap   = 100;
    for (std::size_t i = begin; i < end && i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        if (m.role != MessageRole::tool) continue;
        if (!looks_like_failure(m.content)) continue;

        auto it = by_id.find(m.tool_call_id);
        std::string name = (it != by_id.end()) ? it->second.first
                                               : std::string("(unknown tool)");
        std::string args = (it != by_id.end()) ? brief_args(it->second.first,
                                                            it->second.second)
                                               : std::string();
        if (args.size() > k_arg_cap) args = args.substr(0, k_arg_cap) + "...";

        notes << "- " << name;
        if (!args.empty()) notes << " " << args;
        notes << " -> " << first_line(m.content) << "\n";

        if (++shown >= k_max_notes) {
            notes << "(... more failures elided)\n";
            break;
        }
    }
    return notes.str();
}

// ---- Format the dropped span for the Layer 6 prompt -----------------------

std::string format_span_for_summary(const std::vector<ChatMessage>& msgs,
                                    std::size_t begin, std::size_t end,
                                    int max_chars)
{
    std::ostringstream ss;
    int budget = max_chars;
    for (std::size_t i = begin; i < end && i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        std::string line;
        line += to_string(m.role);
        line += ": ";
        if (!m.content.empty()) line += m.content;
        if (!m.tool_calls.empty()) {
            for (auto& tc : m.tool_calls) {
                line += "\n  [tool_call ";
                line += tc.name;
                line += " ";
                line += tc.arguments;
                line += "]";
            }
        }
        line += "\n";
        if (static_cast<int>(line.size()) > budget) {
            ss << line.substr(0, static_cast<std::size_t>(std::max(0, budget)));
            ss << "\n... [span truncated for summary prompt] ...\n";
            break;
        }
        ss << line;
        budget -= static_cast<int>(line.size());
    }
    return ss.str();
}

} // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

bool CompactionPipeline::is_preserved(const std::vector<ChatMessage>& msgs,
                                      std::size_t idx,
                                      const CompactionLayerSelection& cfg,
                                      std::size_t first_user_idx,
                                      std::size_t keep_recent_from,
                                      std::size_t last_assistant_text_idx)
{
    if (idx >= msgs.size()) return true;
    const auto& m = msgs[idx];
    if (m.role == MessageRole::system) return true;
    if (idx == first_user_idx) return true;
    if (idx == last_assistant_text_idx) return true;  // S6.18 Task B.1
    if (idx >= keep_recent_from) return true;

    if (m.role == MessageRole::user) {
        if (m.content.find("[Attached file: ") != std::string::npos)
            return true;
        if (cfg.preserve_short_user_msgs_max_tokens > 0) {
            int tok = TokenCounter::estimate_message(m);
            if (tok <= cfg.preserve_short_user_msgs_max_tokens)
                return true;
        }
    }
    if (m.role == MessageRole::assistant && !m.tool_calls.empty()) {
        // Always preserve plan-mode tool calls regardless of size: the plan
        // structure is the agent's working memory across many turns, and
        // dropping a propose_plan / mark_step_done message orphans the plan
        // state on the LLM side even though AgentCore still tracks it.
        for (const auto& tc : m.tool_calls) {
            if (tc.name == "propose_plan" || tc.name == "mark_step_done")
                return true;
        }
        if (cfg.preserve_short_tool_calls_max_tokens > 0) {
            int tok = TokenCounter::estimate_message(m);
            if (tok <= cfg.preserve_short_tool_calls_max_tokens)
                return true;
        }
    }
    return false;
}

std::vector<CompactionPipeline::DropSpan>
CompactionPipeline::drop_candidates(const std::vector<ChatMessage>& msgs,
                                    const CompactionLayerSelection& cfg)
{
    std::vector<DropSpan> out;
    if (msgs.empty()) return out;

    std::size_t first_user    = find_first_user(msgs);
    std::size_t recent_cut    = keep_recent_cutoff(msgs, cfg.keep_recent_turns);
    std::size_t last_asst_txt = find_last_assistant_text(msgs);

    // Walk message-aligned turn groups starting at each user message.
    std::vector<std::size_t> boundaries;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].role == MessageRole::user) boundaries.push_back(i);
    }
    boundaries.push_back(msgs.size());  // sentinel for end-of-history

    for (std::size_t b = 0; b + 1 < boundaries.size(); ++b) {
        std::size_t begin = boundaries[b];
        std::size_t end   = boundaries[b + 1];
        if (begin >= recent_cut) continue;        // in the keep-recent window
        if (begin == first_user) continue;        // never drop the first user msg group

        bool any_preserved = false;
        for (std::size_t i = begin; i < end; ++i) {
            if (is_preserved(msgs, i, cfg, first_user, recent_cut, last_asst_txt)) {
                any_preserved = true;
                break;
            }
        }
        if (any_preserved) continue;
        out.push_back({begin, end});
    }
    return out;
}

std::string CompactionPipeline::format_summary_message(const std::string& summary,
                                                       std::size_t turn_span)
{
    std::ostringstream ss;
    ss << "[Summary of " << turn_span << " earlier message"
       << (turn_span == 1 ? "" : "s")
       << " (compacted to free context)]\n\n"
       << summary;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Layer implementations
// ---------------------------------------------------------------------------

namespace {

LayerResult layer1_drop_redundant_tool_results(std::vector<ChatMessage>& msgs)
{
    LayerResult r;
    r.name = "Layer 1 (drop redundant tool results)";

    // Walk forward, accumulate id -> (name, args), tool result idx.
    std::unordered_map<std::string, ToolCallKey> id_to_key;
    std::unordered_map<ToolCallKey,
                       std::vector<std::size_t>,
                       ToolCallKeyHash> groups;

    for (std::size_t i = 0; i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        if (m.role == MessageRole::assistant) {
            for (auto& tc : m.tool_calls)
                id_to_key[tc.id] = {tc.name, normalise_args(tc.arguments)};
        } else if (m.role == MessageRole::tool && !m.tool_call_id.empty()) {
            auto it = id_to_key.find(m.tool_call_id);
            if (it == id_to_key.end()) continue;
            groups[it->second].push_back(i);
        }
    }

    for (auto& [key, idxs] : groups) {
        if (idxs.size() < 2) continue;
        // Keep the last (most recent) result intact; replace earlier ones.
        for (std::size_t k = 0; k + 1 < idxs.size(); ++k) {
            auto idx = idxs[k];
            auto& m = msgs[idx];
            if (m.content.size() <= 80) continue;  // already short, skip
            std::string stub = "[duplicate of later " + key.name +
                               " call -- result elided to save context]";
            m.content = stub;
            m.token_estimate = TokenCounter::estimate_message(m);
            ++r.messages_touched;
        }
    }

    return r;
}

LayerResult layer2_strip_large_tool_bodies(std::vector<ChatMessage>& msgs,
                                           int threshold,
                                           int count_window,
                                           int count_threshold)
{
    LayerResult r;
    r.name = "Layer 2 (strip large tool bodies)";
    if (threshold <= 0 && count_threshold <= 0) return r;

    // First pass: byte-size threshold. The original Pi-style strip.
    if (threshold > 0) {
        for (auto& m : msgs) {
            if (m.role != MessageRole::tool) continue;
            if (m.content.empty()) continue;
            int tok = TokenCounter::estimate(m.content);
            if (tok <= threshold) continue;

            std::string header = "[Tool result stripped to save context (was ~" +
                                 std::to_string(tok) + " tokens). "
                                 "Original archived; re-call the tool if you need the body.]";
            m.content = header;
            m.token_estimate = TokenCounter::estimate_message(m);
            ++r.messages_touched;
        }
    }

    // S6.17 Task B.1 -- count-based heuristic. When the most recent
    // `count_window` rounds carry >= `count_threshold` tool results, strip
    // the older half regardless of individual byte size. Catches the
    // Pass-5 shape where every result was small (filtered run_command
    // outputs) but the count grew unbounded, leaving the byte-threshold
    // pass with nothing to chew on. Default knobs (10 / 12) tuned against
    // the four archived Pass-5 histories.
    if (count_threshold > 0 && count_window > 0) {
        std::vector<std::size_t> tool_indices;
        tool_indices.reserve(msgs.size());
        for (std::size_t i = 0; i < msgs.size(); ++i) {
            if (msgs[i].role == MessageRole::tool && !msgs[i].content.empty())
                tool_indices.push_back(i);
        }
        if (static_cast<int>(tool_indices.size()) >= count_threshold) {
            // Strip the older HALF of the tool-result population, but only
            // the ones not already replaced by the byte-threshold pass
            // above (their content already starts with "[Tool result
            // stripped..."). The "older half" definition is the first
            // `tool_indices.size() / 2` tool results in chronological
            // order, which roughly mirrors "drop older rounds" without
            // disturbing message ordering or assistant/tool pairing.
            const std::size_t strip_until_idx_in_pop = tool_indices.size() / 2;
            for (std::size_t k = 0; k < strip_until_idx_in_pop; ++k) {
                auto& m = msgs[tool_indices[k]];
                if (m.content.rfind("[Tool result stripped", 0) == 0) continue;
                int tok = TokenCounter::estimate(m.content);
                std::string header = "[Tool result stripped to save context (was ~" +
                                     std::to_string(tok) + " tokens; older half "
                                     "of " + std::to_string(tool_indices.size()) +
                                     " tool results in this turn). "
                                     "Original archived; re-call the tool if you need the body.]";
                m.content = header;
                m.token_estimate = TokenCounter::estimate_message(m);
                ++r.messages_touched;
            }
        }
    }

    return r;
}

LayerResult layer3_drop_old_reasoning(std::vector<ChatMessage>& msgs,
                                      int older_than_turns)
{
    LayerResult r;
    r.name = "Layer 3 (drop old reasoning)";
    if (older_than_turns < 0) return r;

    std::size_t recent_cut = keep_recent_cutoff(msgs, older_than_turns);
    for (std::size_t i = 0; i < recent_cut; ++i) {
        auto& m = msgs[i];
        if (m.role != MessageRole::assistant) continue;
        if (m.content.find("<think>") == std::string::npos) continue;
        int before = TokenCounter::estimate_message(m);
        int n = strip_think_blocks(m.content);
        if (n == 0) continue;
        m.token_estimate = TokenCounter::estimate_message(m);
        ++r.messages_touched;
        r.tokens_freed += (before - m.token_estimate);
    }
    return r;
}

LayerResult layer5_drop_oldest_turns(std::vector<ChatMessage>& msgs,
                                     const CompactionLayerSelection& cfg,
                                     int target_tokens,
                                     int current_tokens)
{
    LayerResult r;
    r.name = "Layer 5 (drop oldest turn pairs)";
    if (msgs.empty()) return r;

    // Re-derive candidates after each delete to keep indices valid. We drop
    // oldest-first because the earliest spans are the cheapest to lose.
    int now = current_tokens;
    while (now > target_tokens) {
        auto cands = CompactionPipeline::drop_candidates(msgs, cfg);
        if (cands.empty()) break;
        const auto& s = cands.front();
        int dropped = static_cast<int>(s.end - s.begin);
        msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(s.begin),
                   msgs.begin() + static_cast<ptrdiff_t>(s.end));
        r.messages_touched += dropped;
        now = TokenCounter::estimate(msgs);
    }
    return r;
}

LayerResult layer6_llm_summary(std::vector<ChatMessage>& msgs,
                               const CompactionLayerSelection& cfg,
                               int target_tokens,
                               int current_tokens,
                               ILLMClient* llm,
                               const LLMConfig& llm_config)
{
    LayerResult r;
    r.name = "Layer 6 (LLM summary)";
    if (!llm) {
        r.detail = "no LLM client available";
        return r;
    }
    if (msgs.empty()) return r;

    auto candidates = CompactionPipeline::drop_candidates(msgs, cfg);
    if (candidates.empty()) {
        r.detail = "no drop candidates (everything is preserved)";
        return r;
    }

    // Merge contiguous spans into one summary range (oldest to newest).
    std::size_t span_begin = candidates.front().begin;
    std::size_t span_end   = candidates.back().end;

    // Avoid summarising spans that wouldn't fit any work between them.
    int span_chars_cap = 12000;  // keep the prompt itself in bounds
    std::string conv = format_span_for_summary(msgs, span_begin, span_end,
                                                span_chars_cap);

    std::string prompt = substitute_template(
        k_compaction_summary_template, conv, cfg.custom_summary_instructions);

    // Build a one-shot user message; no tool schemas; bypass the agent loop.
    std::vector<ChatMessage> req;
    req.push_back({MessageRole::system,
                   "You produce structured summaries of partial conversations."});
    req.push_back({MessageRole::user, prompt});

    std::string accumulated;
    bool        had_error = false;
    StreamCallbacks cbs;
    cbs.on_token = [&](const std::string& tok) { accumulated += tok; };
    cbs.on_reasoning_token = [&](const std::string& /*tok*/) {};
    cbs.on_tool_calls = [&](const std::vector<ToolCallRequest>&) {};
    cbs.on_error = [&](const std::string& err) {
        had_error = true;
        spdlog::warn("Layer 6 summary call failed: {}", err);
    };
    cbs.on_should_cancel = []() { return false; };

    LLMConfig summary_cfg = llm_config;
    if (cfg.summary_max_tokens > 0)
        summary_cfg.max_tokens = cfg.summary_max_tokens;

    try {
        llm->stream_completion(req, /*tools*/{}, cbs);
    } catch (const std::exception& e) {
        spdlog::warn("Layer 6: stream_completion threw: {}", e.what());
        had_error = true;
    }

    if (had_error || accumulated.empty()) {
        r.detail = had_error ? "LLM call failed -- layer skipped"
                             : "LLM returned empty body -- layer skipped";
        return r;
    }

    std::size_t span_messages = span_end - span_begin;

    // Deterministic "previous attempts" notes appended to the LLM summary.
    // The LLM summary paraphrases; this preserves the (tool, args, error)
    // tuples literally so the model doesn't re-try the same wrong approach
    // post-compaction. See extract_failure_notes() above.
    std::string failure_notes = extract_failure_notes(msgs, span_begin, span_end);
    std::string composed = accumulated;
    if (!failure_notes.empty()) {
        if (!composed.empty() && composed.back() != '\n') composed += '\n';
        composed += "\nNotes from compacted turns (do not retry these "
                    "exact approaches without changing something first):\n";
        composed += failure_notes;
    }
    std::string summary_msg = CompactionPipeline::format_summary_message(
        composed, span_messages);

    // Replace the entire span with one assistant-role message.
    msgs.erase(msgs.begin() + static_cast<ptrdiff_t>(span_begin),
               msgs.begin() + static_cast<ptrdiff_t>(span_end));
    ChatMessage inserted;
    inserted.role = MessageRole::assistant;
    inserted.content = summary_msg;
    inserted.token_estimate = TokenCounter::estimate_message(inserted);
    msgs.insert(msgs.begin() + static_cast<ptrdiff_t>(span_begin),
                std::move(inserted));

    r.messages_touched = static_cast<int>(span_messages);
    r.summary_text     = std::move(accumulated);
    int after = TokenCounter::estimate(msgs);
    r.tokens_freed     = current_tokens - after;
    (void)target_tokens;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Cascade
// ---------------------------------------------------------------------------

PipelineResult CompactionPipeline::run(ConversationHistory& history,
                                       const CompactionLayerSelection& cfg,
                                       int target_tokens,
                                       ILLMClient* llm,
                                       const LLMConfig& llm_config)
{
    PipelineResult pr;
    pr.target_tokens = target_tokens;

    // Work on a mutable copy of the message vector.
    std::vector<ChatMessage> msgs = history.messages();
    pr.before_tokens = TokenCounter::estimate(msgs);
    int now          = pr.before_tokens;

    auto execute_layer = [&](auto&& fn) {
        LayerResult lr = fn(msgs);
        lr.ran = true;
        int after = TokenCounter::estimate(msgs);
        lr.tokens_freed = std::max(lr.tokens_freed, now - after);
        now = after;
        pr.layers.push_back(std::move(lr));
    };

    if (cfg.drop_redundant_tool_results) {
        execute_layer([&](std::vector<ChatMessage>& m) {
            return layer1_drop_redundant_tool_results(m);
        });
    }

    if (now > target_tokens && cfg.strip_large_tool_bodies) {
        execute_layer([&](std::vector<ChatMessage>& m) {
            return layer2_strip_large_tool_bodies(
                m, cfg.strip_threshold_tokens,
                cfg.count_heuristic_window,
                cfg.count_heuristic_threshold);
        });
    }

    if (now > target_tokens && cfg.drop_old_reasoning) {
        execute_layer([&](std::vector<ChatMessage>& m) {
            return layer3_drop_old_reasoning(m, cfg.older_than_turns);
        });
    }

    if (now > target_tokens && cfg.drop_oldest_turns) {
        execute_layer([&](std::vector<ChatMessage>& m) {
            return layer5_drop_oldest_turns(m, cfg, target_tokens, now);
        });
    }

    // S6.18 Task B.1 -- when the cascade hasn't reached target by this point,
    // fire Layer 6 unconditionally (relax the `cfg.llm_summary` gate). The
    // progress sentinel injected by AgentCore reads its "Progress so far:"
    // body from the summary the layer produces; without that body the
    // post-compaction model sees an empty breadcrumb. Only the user toggling
    // off the entire LLM (no `llm` pointer) skips this branch.
    if (now > target_tokens && llm) {
        execute_layer([&](std::vector<ChatMessage>& m) {
            return layer6_llm_summary(m, cfg, target_tokens, now, llm, llm_config);
        });
    }

    pr.after_tokens    = now;
    pr.reached_target  = (now <= target_tokens);

    // Surface the Layer 6 summary text (verbatim from the LLM, without the
    // appended failure-notes block) so AgentCore can inject it into the
    // index-1 footnote.
    for (const auto& lr : pr.layers) {
        if (!lr.summary_text.empty()) {
            pr.llm_summary_text = lr.summary_text;
            break;
        }
    }

    // Commit the mutated message vector back to history. We replace the
    // entire vector; ConversationHistory's owner-thread fence asserts this
    // runs on the agent thread (caller wraps in ConversationOwnerScope).
    history.clear();
    for (auto& m : msgs) history.add(std::move(m));

    return pr;
}

} // namespace locus

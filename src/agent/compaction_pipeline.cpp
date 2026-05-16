#include "compaction_pipeline.h"
#include "compaction_prompts.h"

#include "llm/token_counter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>

namespace locus {

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
                                      std::size_t keep_recent_from)
{
    if (idx >= msgs.size()) return true;
    const auto& m = msgs[idx];
    if (m.role == MessageRole::system) return true;
    if (idx == first_user_idx) return true;
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

    std::size_t first_user = find_first_user(msgs);
    std::size_t recent_cut = keep_recent_cutoff(msgs, cfg.keep_recent_turns);

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
            if (is_preserved(msgs, i, cfg, first_user, recent_cut)) {
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
                                           int threshold)
{
    LayerResult r;
    r.name = "Layer 2 (strip large tool bodies)";
    if (threshold <= 0) return r;

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
    std::string summary_msg = CompactionPipeline::format_summary_message(
        accumulated, span_messages);

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
            return layer2_strip_large_tool_bodies(m, cfg.strip_threshold_tokens);
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

    if (now > target_tokens && cfg.llm_summary && llm) {
        execute_layer([&](std::vector<ChatMessage>& m) {
            return layer6_llm_summary(m, cfg, target_tokens, now, llm, llm_config);
        });
    }

    pr.after_tokens    = now;
    pr.reached_target  = (now <= target_tokens);

    // Commit the mutated message vector back to history. We replace the
    // entire vector; ConversationHistory's owner-thread fence asserts this
    // runs on the agent thread (caller wraps in ConversationOwnerScope).
    history.clear();
    for (auto& m : msgs) history.add(std::move(m));

    return pr;
}

} // namespace locus

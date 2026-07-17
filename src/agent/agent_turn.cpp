#include "agent_turn.h"

#include "agent_core.h"
#include "agent_loop.h"
#include "build_error_detector.h"
#include "compaction_pipeline.h"
#include "convergence_signals.h"
#include "quality_monitor.h"
#include "tools/shared.h"
#include "tools/tool.h"
#include "../core/workspace.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <utility>

namespace locus {

AgentTurnRunner::AgentTurnRunner(AgentCore& core, int turn_id, int max_rounds)
    : core_(core), turn_id_(turn_id), max_rounds_(max_rounds)
{}

AgentTurnRunner::Outcome AgentTurnRunner::run()
{
    Outcome out;
    int& round = out.rounds_run;
    bool& turn_had_error = out.turn_had_error;

    auto under_cap = [&]() {
        return max_rounds_ <= 0 || round < max_rounds_;
    };

    while (under_cap()) {
        if (core_.cancel_requested_.load()) {
            spdlog::info("AgentCore: turn cancelled by user");
            core_.frontends_.broadcast([](IFrontend& fe) {
                fe.on_error("Turn cancelled.");
            });
            break;
        }

        ++round;
        // Round bookend at info level (was trace, bumped after agentic
        // testing showed the chronology was unreadable at info without
        // -verbose). Paired with the LLM POST summary line later in the
        // same round, plus the per-tool dispatch lines, this gives a
        // greppable round-by-round timeline in .locus/locus.log.
        spdlog::info("AgentCore: round {} start (turn {})", round, turn_id_);
        core_.frontends_.broadcast([&](IFrontend& fe) {
            fe.on_round_progress(round, max_rounds_);
        });

        core_.apply_pending_compaction();
        core_.apply_pending_mode_change();
        core_.apply_pending_plan_decision();
        core_.apply_pending_deletes();

        // Mid-turn user-message injection: while we were running the previous
        // round, the user may have queued more prompts. Drain non-slash
        // entries from the front of `message_queue_` and append them as
        // user messages so the next LLM call sees them. Slash commands
        // ('/clear', '/compact', ...) are NOT injected -- they need full
        // turn-boundary semantics (history mutation, dialog spawn, ...) so
        // we stop at the first slash entry and let the natural pop-and-
        // process loop in agent_thread_func handle them once this turn ends.
        // Cancellation also short-circuits the drain so a Stop-mid-turn
        // doesn't sneak a queued prompt into history.
        if (!core_.cancel_requested_.load()) {
            std::vector<std::string> injected;
            {
                std::lock_guard lock(core_.queue_mutex_);
                while (!core_.message_queue_.empty()) {
                    const std::string& front = core_.message_queue_.front();
                    std::size_t i = 0;
                    while (i < front.size()
                           && std::isspace(static_cast<unsigned char>(front[i])))
                        ++i;
                    if (i < front.size() && front[i] == '/') break;
                    injected.push_back(std::move(core_.message_queue_.front()));
                    core_.message_queue_.pop();
                }
            }
            for (auto& m : injected) {
                spdlog::info("AgentCore: injecting queued user message mid-turn "
                             "({} chars, round {})", m.size(), round);
                int chars = static_cast<int>(m.size());
                core_.ctx_->add_message({MessageRole::user, m});
                int tok = core_.ctx_->history().messages().back().token_estimate;
                core_.activity_->emit(ActivityKind::user_message,
                                "User message (mid-turn, round "
                                    + std::to_string(round) + ", "
                                    + std::to_string(chars) + " chars)",
                                m,
                                /*tokens_in=*/tok);
            }
            if (!injected.empty()) {
                core_.frontends_.broadcast([&](IFrontend& fe) {
                    fe.on_context_meter(core_.ctx_->current_tokens(),
                                        core_.ctx_->llm_config().context_limit,
                                        core_.ctx_->budget().last_prompt_tokens(),
                                        core_.ctx_->budget().last_completion_tokens(),
                                        core_.ctx_->budget().reserve());
                });
            }
        }

        // S5.F -- auto-compact band. When usage crosses the configured
        // auto_threshold of effective_limit and auto_enabled is on, queue
        // the cascade (1+2+3+6, layer 5 reserved for explicit user invocation)
        // and apply it immediately so the next round runs with a leaner history.
        if (auto* ws = core_.services_.workspace()) {
            const auto& cc = ws->config().compaction;
            // Only consider the auto band when the LLM context window is
            // actually known (auto-detected > 0). Otherwise effective_limit
            // collapses to 1 and a 2-K system prompt looks like 200000% usage.
            int  raw_limit = core_.ctx_->llm_config().context_limit;
            if (cc.auto_enabled && raw_limit > 0) {
                int eff  = core_.ctx_->effective_limit();
                int used = core_.ctx_->current_tokens();
                if (eff > 0 && used > 0) {
                    double ratio = static_cast<double>(used) / eff;
                    if (ratio >= cc.auto_threshold) {
                        CompactionLayerSelection sel = selection_from_config(cc);
                        int target = static_cast<int>(eff * cc.warn_threshold);
                        if (target <= 0) target = eff / 2;
                        spdlog::warn("AgentCore: auto-compact firing ({} / {} = {:.0f}% of effective_limit, threshold {:.0f}%)",
                                     used, eff, ratio * 100, cc.auto_threshold * 100);
                        {
                            std::lock_guard lock(core_.queue_mutex_);
                            core_.pending_pipeline_           = true;
                            core_.pending_pipeline_selection_ = sel;
                            core_.pending_pipeline_target_    = target;
                            core_.pending_pipeline_overrides_.clear();
                        }
                        core_.apply_pending_compaction();
                    }
                }
            }
        }

        if (core_.ctx_->budget().check_overflow(core_.ctx_->current_tokens()))
            break;

        // S6.21 Task 4 -- if a hard-trim fired this round (either the start-of-
        // round drained pipeline OR the auto-compact band above), re-inject a
        // build-loop breadcrumb before the next LLM call so a small-context
        // model doesn't lose WHICH error it was mid-fixing.
        maybe_inject_compaction_breadcrumb();

        ToolMode tool_mode = ToolMode::agent;
        switch (core_.mode_.load(std::memory_order_acquire)) {
        case AgentMode::chat:    tool_mode = ToolMode::agent;   break;
        case AgentMode::plan:    tool_mode = ToolMode::plan;    break;
        case AgentMode::execute: tool_mode = ToolMode::execute; break;
        }
        auto step = core_.loop_->run_step(core_.ctx_->history(), tool_mode);

        core_.frontends_.broadcast([&](IFrontend& fe) {
            fe.on_context_meter(core_.ctx_->current_tokens(),
                                core_.ctx_->llm_config().context_limit,
                                core_.ctx_->budget().last_prompt_tokens(),
                                core_.ctx_->budget().last_completion_tokens(),
                                core_.ctx_->budget().reserve(),
                                step.stream_ms);
        });

        if (step.had_error) {
            // S6.21 Task 1 -- the all-dropped case surfaces here: when every
            // tool call this round was malformed, LMStudioClient fires on_error
            // (so had_error is set) and delivers no calls. Treat it as a
            // steer-or-abort loop rather than a hard turn-ending error, so the
            // model gets one chance to re-send a well-formed call. Any other
            // error (network, reserve breach, ...) falls through to the break.
            if (step.delivered_tool_calls == 0 && step.dropped_tool_calls > 0) {
                bool should_break = false;
                check_all_dropped(step, should_break);
                if (should_break) {
                    turn_had_error = true;
                    break;
                }
                continue;  // nudge injected; re-enter the round loop
            }
            turn_had_error = true;
            break;
        }

        // S6.21 Task 1 -- a successful round with at least one delivered tool
        // call (or a plain text round) resets the all-dropped streak. Done
        // unconditionally here so the streak only ever counts CONSECUTIVE
        // all-dropped rounds.
        if (step.delivered_tool_calls > 0) {
            all_dropped_nudged_ = false;
        }

        // Skip useless assistant messages -- empty content AND no tool_calls
        // means the LLM returned nothing actionable. Most often happens on a
        // mid-stream cancel before any token arrived, or on an LM Studio
        // stream that died after finish_reason but before content. Appending
        // such a message would leave a permanent orphan in history that has
        // no UI bubble (render_loaded_history skips empty-content assistants)
        // and corrupts the [user, assistant, ...] alternation some local LLM
        // jinja templates require -- LM Studio's "No user query found in
        // messages" rejection is the symptom.
        const bool useless = step.assistant_msg.content.empty()
                          && step.assistant_msg.tool_calls.empty();

        if (core_.cancel_requested_.load()) {
            // S6.13 -- distinguish three reasons the cancel flag is hot:
            //   (a) user clicked Stop                       -> abort turn
            //   (b) UI clicked Commit now                   -> nudge + continue
            //   (c) AgentLoop's auto-nudge tripped the flag -> nudge + continue
            const bool is_nudge =
                core_.nudge_requested_.exchange(false) || step.watchdog_auto_nudge;
            if (is_nudge) {
                if (++core_.nudges_this_turn_ > 2) {
                    spdlog::warn("AgentCore: 3 reasoning watchdog trips in one turn; aborting");
                    core_.frontends_.broadcast([](IFrontend& fe) {
                        fe.on_error(
                            "Agent appears stuck (3 reasoning watchdog trips "
                            "in one turn). Stopping. Adjust the prompt or raise "
                            "agent.reasoning_max_chars / reasoning_max_seconds "
                            "thresholds if the model genuinely needs more room.");
                        fe.on_reasoning_watchdog_cleared();
                    });
                    break;
                }
                // Soft cancel: clear the abort flag, append the steering
                // message, and continue the round loop. The new round will
                // re-enter the LLM with the message visible in history.
                core_.cancel_requested_.store(false);
                std::string nudge =
                    "[Auto-nudge] Stop reasoning. Commit to a tool call now, "
                    "or give a brief final answer if no tool is needed.";
                core_.ctx_->add_message({MessageRole::user, nudge});
                int tok = core_.ctx_->history().messages().back().token_estimate;
                core_.activity_->emit(
                    ActivityKind::user_message,
                    "Reasoning watchdog nudge (#" + std::to_string(core_.nudges_this_turn_)
                        + ", trigger=" + (step.watchdog_trigger.empty()
                                              ? std::string("manual")
                                              : step.watchdog_trigger) + ")",
                    nudge,
                    /*tokens_in=*/tok);
                core_.frontends_.broadcast([](IFrontend& fe) {
                    fe.on_reasoning_watchdog_cleared();
                });
                spdlog::info("AgentCore: reasoning-watchdog nudge #{} injected "
                             "(trigger={})", core_.nudges_this_turn_,
                             step.watchdog_trigger.empty() ? "manual"
                                                            : step.watchdog_trigger);
                continue;  // re-enter while-loop body for next round
            }

            if (!useless)
                core_.ctx_->add_message(std::move(step.assistant_msg));
            spdlog::info("AgentCore: turn cancelled mid-stream");
            core_.frontends_.broadcast([](IFrontend& fe) {
                fe.on_error("Turn cancelled.");
                fe.on_reasoning_watchdog_cleared();
            });
            break;
        }

        if (!useless)
            core_.ctx_->add_message(std::move(step.assistant_msg));

        // S6.10 Task B -- empty_response detector. Fires when the assistant
        // returned no content / no tool_calls / no reasoning. The model has
        // no failure mode to react to without an explicit nudge: bail with
        // useless=true would otherwise just end the turn silently.
        if (useless) {
            bool quality_on = true;
            if (auto* ws = core_.services_.workspace())
                quality_on = ws->config().agent.quality_monitor_enabled;
            if (quality_on) {
                auto correction = QualityMonitor::evaluate(
                    core_.ctx_->history().messages());
                if (correction && correction->kind ==
                    QualityCorrectionKind::empty_response)
                {
                    if (!core_.inject_nudge(correction->corrective_message,
                                            "empty_response"))
                    {
                        core_.frontends_.broadcast([](IFrontend& fe) {
                            fe.on_error(
                                "Agent appears stuck (quality monitor "
                                "exceeded its 2-correction cap this turn).");
                        });
                        break;
                    }
                    continue;  // re-enter the round loop
                }
            }
            break;  // nothing to do; let the turn complete naturally
        }

        if (step.tool_calls.empty()) {
            // S6.21 Task 3 -- a turn-ending text answer. This round dispatched
            // no tools, so a "build succeeds / frame0 created" claim here is
            // ungrounded unless a PRIOR round in the turn verified it. We pass
            // this round's (empty) tool names; the predicate's phrase list is
            // conservative enough that an honest end-of-turn summary referencing
            // an earlier successful build won't trip it on a generic word.
            check_unverified_success(step.assistant_msg.content, {});
            break;
        }

        // S6.10 Task B -- repeated_tool_call detector. Inspect the latest
        // assistant message (just appended) BEFORE dispatching its tool
        // calls. If it repeats the previous (name, args), we know the model
        // is in a tight loop and the dispatch will not produce new info.
        // Inject a corrective and continue without running the tool again.
        {
            bool quality_on = true;
            if (auto* ws = core_.services_.workspace())
                quality_on = ws->config().agent.quality_monitor_enabled;
            if (quality_on) {
                auto correction = QualityMonitor::evaluate(
                    core_.ctx_->history().messages());
                if (correction && correction->kind ==
                    QualityCorrectionKind::repeated_tool_call)
                {
                    if (!core_.inject_nudge(correction->corrective_message,
                                            "repeated_tool_call"))
                    {
                        core_.frontends_.broadcast([](IFrontend& fe) {
                            fe.on_error(
                                "Agent appears stuck (quality monitor "
                                "exceeded its 2-correction cap this turn).");
                        });
                        break;
                    }
                    continue;
                }
            }
        }

        std::string round_tool_output;   // accumulates this round's tool results
        for (auto& call : step.tool_calls) {
            if (core_.cancel_requested_.load()) {
                spdlog::info("AgentCore: skipping remaining tool calls (cancelled)");
                break;
            }
            std::string captured_content;
            bool        captured_success = true;
            note_write_call(call);
            core_.dispatcher_->dispatch(call, [&](ChatMessage m) {
                captured_content = m.content;
                core_.ctx_->add_message(std::move(m));
            });
            if (!round_tool_output.empty()) round_tool_output += '\n';
            round_tool_output += captured_content;
            // S6.21 Task 2 -- per-result edit_file ambiguity / not-found loop
            // detector. Independent of the build-error streak (which keys on
            // compiler output, not tool errors).
            check_edit_ambiguity(captured_content);
            core_.observe_plan_tool_result(call, captured_content, captured_success);
        }

        {
            std::lock_guard lock(core_.plan_mutex_);
            if (core_.plan_awaiting_decision_)
                break;
        }

        // S6.20 -- stuck-on-the-same-build-error detector. Runs on this round's
        // aggregated tool output (run_command build results land here). Fires a
        // nudge on the Kth identical error signature, aborts if it persists.
        {
            bool should_break = false;
            check_stuck_error(round_tool_output, should_break);
            if (should_break) break;
        }
    }

    if (max_rounds_ > 0 && round >= max_rounds_) {
        turn_had_error = true;
        spdlog::warn("AgentCore: hit max round limit ({})", max_rounds_);
        const int hit = max_rounds_;
        core_.frontends_.broadcast([hit](IFrontend& fe) {
            fe.on_error("Agent reached the maximum number of tool call "
                        "rounds (" + std::to_string(hit) +
                        "). Raise agent.max_rounds_per_message in "
                        ".locus/config.json if the task legitimately needs "
                        "more (set to 0 to remove the cap).");
        });
    }

    return out;
}

void AgentTurnRunner::check_stuck_error(const std::string& tool_output,
                                        bool& out_break)
{
    // Honour the same workspace switch the other quality detectors use -- this
    // is a quality-monitor-class behaviour.
    if (auto* ws = core_.services_.workspace()) {
        if (!ws->config().agent.quality_monitor_enabled) return;
    }

    std::string sig = extract_error_signature(tool_output);
    if (sig.empty()) {
        // Reset ONLY when the round shows a command that actually succeeded
        // ([exit code: 0] from run_command) -- genuine progress. The canonical
        // thrash pattern alternates edit_file (clean round) with a failing
        // build (error round); resetting on every clean round kept the streak
        // pinned at 1 so the detector never fired on exactly the loop it was
        // built for (2026-07-04 agentic gl_cube finding). Pure edit / read
        // rounds leave the streak frozen; an unrelated later error still
        // replaces the signature via the sig != last branch below.
        // Line-anchored so a read_file result that merely quotes the string
        // "[exit code: 0]" from some log file doesn't spuriously reset the
        // streak -- run_command emits it as its own line (process_tools.cpp).
        if (tool_output.rfind("[exit code: 0]", 0) == 0 ||
            tool_output.find("\n[exit code: 0]") != std::string::npos) {
            last_error_sig_.clear();
            same_error_streak_ = 0;
            stuck_nudge_fired_ = false;
        }
        return;
    }

    if (sig == last_error_sig_) {
        ++same_error_streak_;
    } else {
        last_error_sig_   = sig;
        same_error_streak_ = 1;
        stuck_nudge_fired_ = false;
    }

    // A build error just landed. If the model got here by overwriting a large
    // file wholesale, nudge it toward targeted edits ONCE -- this often heads
    // off the streak before it needs the harder stuck-error nudge/abort.
    maybe_hint_large_overwrite();

    if (same_error_streak_ < k_stuck_error_streak) return;

    // One-line preview of the repeating error for the log + activity.
    std::string preview = sig.substr(0, sig.find('\n'));

    if (!stuck_nudge_fired_) {
        // First time we cross the streak: nudge the model to change approach.
        std::string nudge =
            "[Build loop] You have hit the SAME build error " +
            std::to_string(same_error_streak_) + " rounds in a row:\n  " +
            preview + "\n"
            "Rewriting the whole file is not fixing it. STOP and change approach: "
            "read the exact failing line(s) with read_file, then make ONE minimal, "
            "targeted edit_file change that addresses this specific error -- do not "
            "rewrite unrelated code. If you are unsure what is wrong, say so briefly "
            "instead of guessing again.";
        spdlog::warn("AgentCore: stuck on repeating build error ({}x): {}",
                     same_error_streak_, preview);
        if (!core_.inject_nudge(nudge, "stuck_build_error")) {
            // Nudge cap already exhausted by other detectors -> abort now.
            core_.frontends_.broadcast([](IFrontend& fe) {
                fe.on_error("Agent appears stuck on a repeating build error "
                            "(correction cap reached). Stopping.");
            });
            out_break = true;
            return;
        }
        stuck_nudge_fired_ = true;
        return;  // give the nudge a round to take effect
    }

    // We already nudged for this signature and it STILL recurs -> abort.
    spdlog::warn("AgentCore: still stuck on the same build error after a nudge "
                 "({}x): {}; aborting turn", same_error_streak_, preview);
    core_.frontends_.broadcast([&preview](IFrontend& fe) {
        fe.on_error("Agent appears stuck on a repeating build error -- the same "
                    "compile error recurred after a steering hint:\n  " + preview +
                    "\nStopping. Try a tighter prompt that points at the exact "
                    "fix, or fix that error manually.");
    });
    out_break = true;
}

void AgentTurnRunner::note_write_call(const ToolCall& call)
{
    // S6.21 Task 4 -- remember the last file the model wrote/edited this turn so
    // the compaction breadcrumb can name it. edit_file uses file_path (canonical)
    // or path (legacy); write_file / delete_file use path or file_path.
    if (call.tool_name == "edit_file"
     || call.tool_name == "write_file"
     || call.tool_name == "delete_file") {
        std::string p = call.args.value("file_path", "");
        if (p.empty()) p = call.args.value("path", "");
        if (!p.empty()) last_edited_file_ = p;
    }

    // Only a write_file that OVERWRITES a large existing file is the thrash
    // signature. A plain create (overwrite=false) or a small file is fine.
    if (call.tool_name != "write_file") return;
    if (!tools::coerce_bool(call.args, "overwrite", false)) return;
    auto it = call.args.find("content");
    if (it == call.args.end() || !it->is_string()) return;
    if (it->get<std::string>().size() < k_large_overwrite_bytes) return;
    pending_large_overwrite_ = true;
}

void AgentTurnRunner::maybe_hint_large_overwrite()
{
    if (!pending_large_overwrite_) return;
    pending_large_overwrite_ = false;       // consume: only react to the next build
    if (large_overwrite_hinted_) return;    // once per turn

    if (auto* ws = core_.services_.workspace()) {
        if (!ws->config().agent.quality_monitor_enabled) return;
    }

    std::string hint =
        "[Edit strategy] You rewrote a large file wholesale and the build still "
        "fails. Rewriting the entire file each round re-introduces errors and "
        "wastes the context budget. Prefer edit_file with a small, targeted "
        "old_string/new_string change that fixes ONLY the reported error, instead "
        "of another full write_file overwrite.";
    if (core_.inject_nudge(hint, "large_overwrite")) {
        large_overwrite_hinted_ = true;
        spdlog::info("AgentCore: hinted edit_file over large write_file overwrite");
    }
    // If inject_nudge returned false (cap exhausted) we just skip the hint --
    // the stuck-error path will handle the abort.
}

void AgentTurnRunner::check_all_dropped(const AgentStepResult& step,
                                        bool& out_break)
{
    if (auto* ws = core_.services_.workspace()) {
        if (!ws->config().agent.quality_monitor_enabled) {
            // Quality monitor off: don't intercept. The caller already set
            // turn_had_error and will break on the underlying error.
            out_break = true;
            return;
        }
    }

    AllDroppedAction action = classify_all_dropped(
        step.delivered_tool_calls, step.dropped_tool_calls, all_dropped_nudged_);

    // Defensive: classify_all_dropped only returns nudge/abort for the
    // all-dropped case, which the caller already gated on. `none` shouldn't
    // happen here, but if it does, fall back to the hard error break.
    if (action == AllDroppedAction::none) {
        out_break = true;
        return;
    }

    std::string diag = step.dropped_diagnostic.empty()
        ? std::string("the model's tool call arguments were not valid JSON")
        : step.dropped_diagnostic;

    if (action == AllDroppedAction::abort) {
        spdlog::warn("AgentCore: tool calls repeatedly malformed "
                     "({} dropped, 0 delivered) after a nudge; aborting turn",
                     step.dropped_tool_calls);
        core_.frontends_.broadcast([&diag](IFrontend& fe) {
            fe.on_error("Agent appears stuck (tool calls repeatedly malformed). "
                        "Last failure: " + diag +
                        ". Stopping. Try a tighter prompt, a model with cleaner "
                        "tool-call output, or lower llm.max_tokens pressure.");
        });
        out_break = true;
        return;
    }

    // First all-dropped round: nudge once.
    std::string nudge =
        "[Malformed tool call] Your last tool call's arguments were not valid "
        "JSON (" + diag + "). Re-send the SAME tool call but with a correctly "
        "quoted, well-formed JSON object for its arguments -- do not repeat the "
        "previous malformed call verbatim, and keep the arguments compact so the "
        "response is not truncated.";
    if (!core_.inject_nudge(nudge, "all_tool_calls_dropped")) {
        // Shared nudge cap already exhausted by another detector -> abort now.
        core_.frontends_.broadcast([](IFrontend& fe) {
            fe.on_error("Agent appears stuck (tool calls repeatedly malformed; "
                        "correction cap reached). Stopping.");
        });
        out_break = true;
        return;
    }
    all_dropped_nudged_ = true;
    spdlog::info("AgentCore: nudged after an all-dropped tool-call round "
                 "({} dropped)", step.dropped_tool_calls);
}

void AgentTurnRunner::check_edit_ambiguity(const std::string& tool_output)
{
    if (auto* ws = core_.services_.workspace()) {
        if (!ws->config().agent.quality_monitor_enabled) return;
    }

    if (!is_edit_ambiguity_error(tool_output)) {
        // A non-ambiguity edit result (or any other tool output) breaks the
        // streak -- the model either landed the edit or moved on.
        edit_ambiguity_streak_ = 0;
        return;
    }

    ++edit_ambiguity_streak_;
    if (edit_ambiguity_streak_ < k_edit_ambiguity_nudge) return;
    if (edit_ambiguity_hinted_) return;  // once per turn

    std::string hint =
        "[Edit anchor] edit_file keeps failing to locate or uniquely match the "
        "old_string. Stop re-guessing the anchor. Either: (1) read_file the exact "
        "region and copy the text VERBATIM including indentation and whitespace; "
        "(2) if the snippet legitimately appears more than once and you want them "
        "all changed, set replace_all=true; or (3) for a large or structural "
        "change, use write_file with overwrite=true to replace the whole file.";
    if (core_.inject_nudge(hint, "edit_ambiguity")) {
        edit_ambiguity_hinted_ = true;
        spdlog::info("AgentCore: hinted edit_file escape hatch after {} "
                     "ambiguity errors this turn", edit_ambiguity_streak_);
    }
    // inject_nudge==false (cap exhausted) -> skip; other detectors own the abort.
}

void AgentTurnRunner::check_unverified_success(
    const std::string& assistant_text,
    const std::vector<std::string>& round_tool_names)
{
    if (auto* ws = core_.services_.workspace()) {
        if (!ws->config().agent.quality_monitor_enabled) return;
    }

    if (!claims_unverified_success(assistant_text, round_tool_names)) return;

    // Non-blocking visibility tripwire: no nudge, no abort. Surface it on the
    // activity stream + a footer note so the agentic get_chat_status / activity
    // scrape (and a human watching) doesn't trust an ungrounded "it builds".
    std::string note =
        "Assistant claimed a verified outcome (build / run / artifact) without a "
        "confirming tool call this turn. Treat the claim as unverified.";
    spdlog::warn("AgentCore: {}", note);
    core_.activity_->emit(ActivityKind::warning,
                          "Unverified success claim", note);
    core_.frontends_.broadcast([&note](IFrontend& fe) {
        fe.on_unverified_success(note);
    });
}

void AgentTurnRunner::maybe_inject_compaction_breadcrumb()
{
    // Read-and-clear the hard-trim flag. Always consume it so a later round
    // doesn't re-fire on a stale trim.
    if (!core_.compaction_hard_trimmed_) return;
    core_.compaction_hard_trimmed_ = false;

    if (auto* ws = core_.services_.workspace()) {
        if (!ws->config().agent.quality_monitor_enabled) return;
    }

    // Only meaningful mid-build-loop: drop the breadcrumb if no build error is
    // live (pure-chat compaction is unaffected).
    if (last_error_sig_.empty()) return;

    // One-line head of the (possibly multi-line) signature -- enough to remind
    // the model which failure it was closing without re-bloating the context.
    std::string sig_head = last_error_sig_.substr(0, last_error_sig_.find('\n'));

    std::string crumb = "[Resuming after context compaction: the last build "
                        "error was `" + sig_head + "`";
    if (!last_edited_file_.empty())
        crumb += "; you were editing `" + last_edited_file_ + "`";
    crumb += ". Make a minimal, targeted edit to fix this specific error -- do "
             "not restart from scratch or rewrite unrelated code.]";

    // Inject as a plain user message (does NOT consume the shared nudge cap --
    // this is context restoration, not a correction). Splices at the tail like
    // any mid-turn user message so it rides the next LLM call.
    core_.ctx_->add_message({MessageRole::user, crumb});
    int tok = core_.ctx_->history().messages().back().token_estimate;
    core_.activity_->emit(ActivityKind::user_message,
                          "Compaction breadcrumb (build-loop resume)",
                          crumb,
                          /*tokens_in=*/tok);
    spdlog::info("AgentCore: injected post-compaction build-loop breadcrumb "
                 "(sig='{}', file='{}')", sig_head, last_edited_file_);
}

} // namespace locus

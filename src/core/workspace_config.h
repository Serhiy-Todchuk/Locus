#pragma once

// WorkspaceConfig -- the per-workspace knob surface. Lives in its own header
// (split from workspace.h) so this gravity well of config fields doesn't drag
// the entire workspace ownership graph into every translation unit that just
// wants to read a setting.
//
// Fields are grouped into nested sub-structs by domain (index / llm / git /
// agent / memory / chat / compaction / sessions / notifications / capabilities).
// The JSON serialiser in workspace_config_json.cpp owns the round-trip; the
// JSON shape mirrors these sub-structs so the C++ surface and the on-disk
// `.locus/config.json` are easy to navigate side by side.
//
// One field stays at top level: `tool_approval_policies`. The JSON shape has
// it as a top-level `tool_approvals` map, and pushing it into a sub-struct
// just for the sake of grouping would force a JSON migration with no payoff.

#include "tools/tool.h"  // for ToolApprovalPolicy

#include <string>
#include <unordered_map>
#include <vector>

namespace locus {

struct WorkspaceConfig {
    // Workspace index + semantic search.
    struct Index {
        std::vector<std::string> exclude_patterns = {
            "node_modules/**", ".git/**", "*.lock", "dist/**",
            "build/**", ".locus/**", ".vs/**"
        };
        int max_file_size_kb = 1024;
        bool code_parsing_enabled = true;

        // S4.L -- when true, the indexer reads `.gitignore` files at workspace
        // open and on watcher-driven `.gitignore` changes, and merges those
        // patterns into its exclude set. Off-switch for the rare workspace that
        // wants `.gitignore`'d files indexed (e.g. archived generated docs).
        bool respect_gitignore = true;

        // Semantic search (on by default - gracefully disabled if model missing).
        // Embedding dimension is derived from the loaded GGUF, never from config:
        // swapping models triggers an automatic re-embed on next workspace open.
        bool semantic_search_enabled = true;
        std::string embedding_model = "bge-m3-Q8_0.gguf";
        int chunk_size_lines = 80;
        int chunk_overlap_lines = 10;

        // Reranker (cross-encoder, S4.J). Off by default until the model is
        // present so first-run users without the GGUF aren't blocked. When on,
        // search_semantic fetches reranker_top_k bi-encoder candidates and
        // reranks to the requested max_results. (search_hybrid was retired in
        // ADR-0009; the eval harness still exercises it directly via IndexQuery.)
        bool reranker_enabled = false;
        std::string reranker_model = "bge-reranker-v2-m3-Q8_0.gguf";
        // 20 candidates x ~100ms/rerank in Release on CPU = ~2s wall-clock per
        // search call, which is the practical ceiling before a synchronous
        // tool invocation feels broken. Bump only if you're on a workstation
        // CPU or move to GPU.
        int reranker_top_k = 20;
    };

    // LLM client settings (persisted per-workspace).
    struct Llm {
        std::string endpoint = "http://127.0.0.1:1234";
        std::string model;           // empty = server default
        double      temperature = 0.7;
        int         context_limit = 0; // 0 = auto-detect from server

        // Per-request completion cap. The 8192 default is sized to accommodate
        // a single big-file write_file payload plus a few hundred tokens of
        // reasoning -- 2048 (the old default) routinely truncated multi-tool-
        // call responses on coding tasks, leaving an empty-arguments tool call
        // that poisoned the next round (HTTP 500 from the chat template).
        // Bump higher if your model is happy with longer answers; the server
        // always clamps to its own ceiling regardless of what we send.
        int         max_tokens = 8192;

        // Stream-stall watchdog (ms): abort the request if zero bytes flow for
        // this long. Not a total-request cap -- a reasoning stream that keeps
        // emitting tokens stays connected indefinitely. The 600s default covers
        // both prefill and the (LM-Studio-template-buffered) <think> block on a
        // 30-40B-class thinking model with multi-K-token context on consumer
        // GPU. Bump higher if your hardware needs it; drop to 60000 to match
        // pre-S4 Locus behaviour.
        // Bumped from 600000 to 1800000 after agentic testing showed slow
        // local models (qwen3.6-27b @ ~2 t/s) routinely exceed the old default
        // on tool-call-heavy rounds. See tests/ui_automation/output/agentic_Tetris/findings.md.
        int         timeout_ms = 1800000;

        // S4.N -- tool-call wire format. Stored as a string so a config.json
        // edited by the user reads naturally; parsed via tool_format_from_string.
        // "auto" runs OpenAI JSON tool_calls + Qwen/Claude XML extraction in
        // parallel, which is the right default for unknown models. Override to
        // "openai" / "qwen" / "claude" / "none" for a known model family.
        std::string tool_format = "auto";

        // S4.V Task 7 -- power-user samplers. Defaults of 0 mean "don't include
        // in the request"; the server's per-model defaults remain in effect.
        // Field names match llama.cpp / LM Studio's accepted JSON keys.
        double      top_p          = 0.0;
        int         top_k          = 0;
        double      min_p          = 0.0;
        double      repeat_penalty = 0.0;
        // OpenAI-protocol penalties. Range [-2, 2]; 0.0 sentinel = "don't send".
        // Distinct from `repeat_penalty` (llama.cpp's multiplicative dial);
        // the three compose rather than overlap.
        double      frequency_penalty = 0.0;
        double      presence_penalty  = 0.0;

        // S6.10 Task D -- server-side grammar-constrained decoding for tool calls.
        // String for natural-reading config.json; parsed via grammar_mode_from_string.
        // "off" preserves pre-S6.10 behaviour; "best_effort" attaches a response_format
        // json_schema describing the union of valid tool calls (LM Studio /
        // llama-server / vLLM honour it; servers that don't ignore the field).
        // "strict" reserved for a future server-probe gate; today treats like
        // best_effort.
        std::string grammar_mode = "off";

        // S6.10 Task F -- auto-apply matching preset at workspace open when
        // `preset_name` is empty or "auto" (the new default). Set false to
        // skip the detect entirely. The current preset is otherwise untouched
        // and the user is expected to pick from the Settings -> LLM tab dropdown.
        bool auto_detect_preset = true;
        // Last preset the user picked (or "" / "auto" to engage auto-detect).
        // Stored in `.locus/config.json` so a workspace re-open uses the chosen
        // value without forcing a re-detect.
        std::string preset_name = "";

        // S6.16 -- per-workspace endpoint-profile override. Empty = follow the
        // global store's `active` profile (the common case). A non-empty value
        // pins this workspace to a named profile in
        // <global_dir>/endpoints.json regardless of what other workspaces use.
        // Resolved in LocusSession::load_shared_resources. The legacy flat
        // `endpoint` / `model` / `tool_format` fields above still win as
        // per-workspace overrides on top of the resolved profile (one-way
        // ratchet so the legacy fields can be dropped cleanly in a future stage).
        std::string active_endpoint = "";
    };

    // S4.L per-turn auto-commit. When `auto_commit` is true AND the workspace
    // contains a `.git/` directory AND at least one file mutation happened
    // this turn, AgentCore runs `git add -A && git commit -m
    // "<prefix><agent-summary>"` after the turn yields. Failures surface as a
    // single warning and don't block the agent; the user resolves manually.
    // `commit_branch` (default empty -> use current branch) lets users park
    // agent commits on a side branch; if the named branch doesn't exist, it's
    // created once and a warning is logged. `commit_prefix` is prepended to
    // the commit message body.
    struct Git {
        bool        auto_commit   = false;
        std::string commit_branch = "";
        std::string commit_prefix = "[locus] ";
    };

    // Agent-loop / tool-execution policy. Everything the agent thread reads
    // out of WorkspaceConfig that isn't LLM client config, memory, or chat UI
    // lives here. JSON path: "agent".
    struct Agent {
        // S3.L -- token-cost guardrail. AgentCore logs the per-turn tool-manifest
        // size at info level every turn; emits a warning when the manifest crosses
        // this threshold. 4000 ~= 12% of a 32K context -- a reasonable "we've grown
        // too much" signal once M4 adds ~20 more tools.
        int tool_manifest_warn_tokens = 4000;

        // S6.11 -- lazy tool manifest. When true, the per-turn tool catalog
        // (both the system-prompt "## Available Tools" section AND the OpenAI
        // tools[] API array) collapses to one-line summaries; the model fetches
        // full schemas on demand via the describe_tool meta-tool. Saves ~3.6K
        // tokens per turn on the default capability matrix -- the big
        // context-budget win for any local-LLM workload (16k context or
        // smaller). The arg-shape error path (S6.17 follow-up) injects the
        // canonical schema into every failure response, so the "extra
        // round-trip per first-use" cost from the original ADR is absorbed
        // automatically when the model guesses wrong. Default on as of the
        // 2026-05-28 calibration -- see the "Prompt-cost tuning" key
        // invariant in CLAUDE.md for the empirical justification.
        bool lazy_tool_manifest = true;

        // S6.12 -- system-prompt profile. Controls which prose body the
        // SystemPromptAssembly renders (Rules / Editing / Shell / MSVC). Workspace
        // metadata, LOCUS.md, memory bank, tools section, and format addendum are
        // identical across profiles. Stored as a string for friendly config.json
        // editing; parsed via SystemPromptProfile profile_from_string.
        //
        //   "full"    -- today's verbose prompt (~700-1000 t prose). Default.
        //   "compact" -- load-bearing rules only, no examples (~300 t prose).
        //   "minimal" -- only the invariants a fine-tuned coding agent must be
        //                told explicitly (~80 t prose). Power-user opt-in.
        //
        // Companion to lazy_tool_manifest above. ADR-0007 carries the rationale.
        std::string system_prompt_profile = "full";

        // S6.17 Task H -- top-level prompt-cost preset. Replaces the manual
        // {lazy_tool_manifest, system_prompt_profile, tool_format hint} tuple
        // with a single named choice:
        //
        //   "minimal"  -> lazy_tool_manifest=on,  system_prompt_profile=minimal
        //   "balanced" -> lazy_tool_manifest=on,  system_prompt_profile=compact
        //   "verbose"  -> lazy_tool_manifest=off, system_prompt_profile=full
        //   ""         -> custom: honour the individual flags above (back-compat)
        //
        // The default "" preserves the pre-S6.17 behaviour. Setting the preset
        // overrides the individual flags at the WorkspaceConfig consumption
        // site (see prompt_cost_apply()).
        std::string prompt_cost = "";

        // S6.13 -- reasoning watchdog. Three OR-semantics triggers; each = 0
        // disables that trigger. When any non-zero threshold trips during an LLM
        // round, the watchdog fires its action:
        //
        //   reasoning_auto_nudge = false (default): non-modal "Commit now" button
        //     surfaces in the chat-footer action row. User clicks -> AgentCore
        //     cancels the current LLM stream and starts a new round with an
        //     injected steering message ("Stop reasoning and commit to a tool
        //     call now or give a brief final answer.").
        //   reasoning_auto_nudge = true: same action fires automatically without
        //     waiting for user click. Designed for agentic-harness driving.
        //
        // Hard cap: 2 nudges per user-message turn. A 3rd would-fire aborts the
        // turn with "Agent appears stuck (3 reasoning watchdog trips in one
        // turn)." All defaults 0/false -> watchdog off, no behaviour change.
        int  reasoning_max_seconds       = 0;  // wall-clock seconds since round start
        int  reasoning_max_chars         = 0;  // reasoning-channel chars in current round
        int  reasoning_max_rounds_silent = 0;  // consecutive rounds since last tool call
        bool reasoning_auto_nudge        = false;

        // S4.I -- per-background-process output ring buffer cap. The reader thread
        // appends stdout+stderr until this many bytes are buffered; older bytes
        // are dropped from the front (the LLM is told how many it missed). 256 KB
        // covers a verbose dev-server log between turns without paging the agent
        // thread, and is still cheap (a few processes x 256 KB is negligible).
        int process_output_buffer_kb = 256;

        // S5.Z follow-up -- per-tool wall-clock guardrail. When > 0, the
        // ToolDispatcher arms a watchdog timer for each tool->execute() call;
        // if the tool hasn't returned by then, the cancel_flag is set and a
        // synthesized "tool timed out" result is fed back to the LLM. This is
        // defence-in-depth for the run_command ReadFile hang documented in
        // tests/ui_automation/output/agentic_Tetris/findings.md (findings 7+8) --
        // a buggy tool that pins the agent thread no longer hangs the session.
        // Default 0 = disabled (existing behaviour). Recommended starting value
        // for users hitting the bug: 600 (10 min, well over any normal build).
        int tool_max_runtime_s = 0;

        // Agentic Tetris findings #5 -- hard cap on tool-call rounds per user
        // message. Was a 20 hardcoded constant in AgentCore; surfaced as a knob
        // because small local models on multi-step build-fix loops genuinely
        // need 30+ rounds (agentic Tetris run 2 hit the previous 20-cap at
        // round 20 mid-edit). The agent surfaces "round N/M" in the chat footer
        // while a turn is in flight; when the cap is hit the dispatcher emits
        // "Agent reached the maximum number of tool call rounds." Raise for
        // long-horizon work; the only ceiling is your patience. Set to 0 to
        // remove the cap entirely (the loop still ends naturally when the LLM
        // stops emitting tool calls). 500 is a sanity-net high enough that
        // realistic "read 200 files, edit 50" workloads don't trip it.
        int max_rounds_per_message = 500;

        // Number of head + tail lines kept by default when run_command /
        // read_process_output return output to the LLM without an explicit
        // output_filter_mode. The middle is replaced with a "[... N lines
        // elided ...]" marker that names the trace-log path for recovery.
        // Lowered keeps context tight on chatty build logs; raised approximates
        // "return everything." Set to 0 to disable the smart-truncate default
        // (the LLM still receives the existing 8 KB hard cap on raw run_command
        // output as a backstop). Per-call output_filter_lines overrides this.
        int run_command_truncate_lines = 50;

        // S5.Z follow-up -- when the run_command reader-thread heartbeat fires
        // (reader still draining 30s+ after child exit, which is the inherited-
        // pipe leak symptom), and `procdump.exe` is on PATH, write a full-memory
        // minidump of the current locus_gui process to `.locus/dumps/`. One dump
        // per workspace-session to avoid filling the disk on a chronically stuck
        // workspace; the agent thread isn't blocked by the dump (procdump.exe is
        // a separate process). Opt-in because procdump isn't a default Windows
        // install and the dump can be 1-2 GB.
        bool dump_on_run_command_hang = false;

        // S4.T -- between-turn external file-change awareness. When true, AgentCore
        // snapshots indexed file mtimes at end of each assistant turn; on the next
        // user turn it computes the diff (excluding files the agent itself touched
        // that turn) and prepends a one-line note to the user message so the LLM
        // knows which files changed under it. Default on -- the cost is one cheap
        // SQLite scan per turn and a single line in the prompt.
        bool notify_external_changes = true;

        // S4.A -- edit_file refuses paths that haven't been read via read_file
        // earlier in the session. Mirrors Claude Code's hallucinated-edit
        // mitigation: forces the model to confirm the exact byte content before
        // proposing an old_string/new_string pair. Default on. Disable when the
        // loaded model is trusted (or the workspace's edits are dominated by
        // single-file passes already preceded by a read), trading some safety
        // for ~one fewer round trip per edit.
        bool require_read_before_edit = true;

        // S6.10 Task G -- anti-truncation detector for code writes. When true,
        // WriteFileTool and EditFileTool scan the final body / new_string for
        // elision markers ("// rest of the code", "// ... existing code ...",
        // etc.) and refuse the operation when a phrase is matched. Default on:
        // a false positive means the user re-issues a write (cheap); a false
        // negative means a silently corrupted file (expensive).
        bool detect_write_truncation = true;

        // S6.10 Task B -- detector layer that watches for empty-response and
        // repeated-tool-call failure modes and injects a corrective nudge
        // through AgentCore's existing nudges_this_turn_ + synthetic-user-message
        // path. Shares the 2-cap with the reasoning watchdog. Default on.
        bool quality_monitor_enabled = true;

        // S6.10 Task C -- strip past-turn reasoning_content from the LLM payload.
        // Reasoning blocks are decision scratchpads that add no information once
        // the tool result is back in the conversation. Keeping them past turn N
        // shoots the prefix cache and inflates round N+1's context. Display
        // layer is unchanged: chat panel and activity log still see the full
        // reasoning. Default on.
        bool strip_past_thinking = true;
    };

    // S4.R workspace-scoped memory bank. When enabled, two tools land in the
    // manifest (`add_memory`, `search_memory`), a slot is reserved in the
    // system prompt for pinned + recently-used entries, and `/memorize` is
    // accepted as a slash command. Disable to remove all three surfaces.
    struct Memory {
        bool enabled = true;
        // Token cap on the always-in-context memory slot. All `pinned:true`
        // entries are injected first; the remaining budget is filled with the
        // most-recently-used unpinned entries. Anything dropped from the slot
        // remains searchable via `search_memory`.
        int  in_context_budget_tokens   = 500;
        // GC ceiling: unpinned entries beyond this count are pruned (oldest by
        // creation time first) on workspace open and after every `add_memory`.
        // Pinned entries are never GC'd.
        int  max_entries                = 200;
        // Per-call cap on the bytes a single `search_memory` response can return
        // to the LLM. Caps the total content size, not the count -- a single
        // very long entry can still appear at the top of the response.
        int  search_response_max_tokens = 1500;
        // Recency half-life in days for the soft recency factor applied during
        // hybrid memory search. `0` disables the recency contribution (rely on
        // BM25 + semantic + reranker only). The default of 21 days matches the
        // stage doc: a 21-day-old entry contributes half as much as a fresh one,
        // smoothly decaying. Justified for the memory corpus (preferences /
        // conventions genuinely supersede over time) but deliberately not used
        // for workspace search.
        int  recency_half_life_days     = 21;
    };

    // Chat-panel UI knobs (display-only; no agent-loop semantics).
    struct Chat {
        // S5.D -- per-message token chip in chat. When true, each bubble shows a
        // small grey "(N t)" estimate. Off-switch for users who find it noisy.
        bool show_per_message_tokens = true;

        // S5.C -- inline diff rendering in chat. When `show_diffs` is true,
        // successful `edit_file` / `write_file` / `delete_file` calls render a
        // red/green unified diff in the chat history below the tool bubble.
        // `diff_max_lines` caps the number of diff lines rendered per call
        // before a "(N more lines collapsed)" marker is emitted -- guards against
        // a single huge file write blowing up the chat HTML.
        bool show_diffs        = true;
        int  diff_max_lines    = 200;
        // Lines of unchanged surrounding context shown before and after each
        // change inside an inline diff. 0 collapses everything to just the
        // add/del lines (the pre-S5.Z layout). Default 4 matches `diff -u`.
        int  diff_context_lines = 4;
        // S5.Z #2 -- soft-collapse threshold for write_file diffs. Diffs longer
        // than this many rows render the first N rows inline and fold the rest
        // into a `<details>`/`<summary>` (native HTML toggle, no JS). 0 disables
        // collapsing -- show every row inline.
        int  diff_collapse_threshold = 16;
    };

    // S5.F -- composable compaction. The compaction pipeline runs a fixed-order
    // cascade of layers (drop-redundant-tool-results, strip-large-tool-bodies,
    // drop-old-reasoning, drop-oldest-turns, LLM-summary). Each layer is
    // independently toggleable; layer 5 (mechanical drop) is off in the auto
    // cascade so the user gets a faithful summary first when 1-3 don't free
    // enough.
    //
    // Three-state progressive trigger (against effective_limit = limit - reserve):
    //   warn_threshold     (default 0.70) -> footer chip turns yellow,
    //                                        inline hint in chat
    //   auto_threshold     (default 0.85) -> cascade fires automatically,
    //                                        user notified after the fact
    //   reserve breach (S5.D, ratio 1.0) -> agent loop refuses; user must
    //                                       compact manually
    struct Compaction {
        // S5.D -- minimum response headroom the agent loop guarantees the LLM.
        // Negative value = auto: std::min(context_limit / 5, 4096).
        // When context_limit is 0 (auto-detect not yet done), reserve is 0.
        int    reserve_tokens  = -1;

        bool   auto_enabled    = true;
        double warn_threshold  = 0.70;
        double auto_threshold  = 0.85;

        // S6.17 Task B.3 -- named preset selects the layer enable matrix.
        //   "gentle"      -> drop_redundant + strip_large
        //   "balanced"    -> + drop_old_reasoning + llm_summary  (default)
        //   "aggressive"  -> + drop_oldest_turns
        //   "custom"      -> use the individual layer_* booleans below
        // Empty == "balanced" for backward compatibility.
        std::string aggressiveness = "balanced";

        // Which cascade layers auto-compact runs (and the initial state of
        // the per-layer checkboxes in the /compact dialog). Persisted via
        // the dialog's Save button so the user can tailor the automatic
        // cascade without editing config.json. Defaults match the original
        // hardcoded auto cascade (1+2+3+6). When `aggressiveness != "custom"`
        // these are derived at run time from the named preset (S6.17 B.3).
        bool   layer_drop_redundant_tool_results = true;
        bool   layer_strip_large_tool_bodies     = true;
        bool   layer_drop_old_reasoning          = true;
        bool   layer_drop_oldest_turns           = false;
        bool   layer_llm_summary                 = true;

        // Per-layer knobs (snapshot copied into the pipeline at run time).
        int    strip_threshold_tokens                = 1000;
        int    older_than_turns                      = 3;
        int    keep_recent_turns                     = 3;
        int    summary_max_tokens                    = 1024;
        int    archive_keep_count                    = 5;
        int    preserve_short_user_msgs_max_tokens   = 500;
        int    preserve_short_tool_calls_max_tokens  = 500;

        // S6.17 Task B.1 -- count-based heuristic for layer_strip_large_tool_bodies.
        // When the most recent N rounds carry >= M tool results, strip the older
        // half regardless of individual byte size. Catches the Pass-5 shape
        // where every result was small but the count was large.
        int    count_heuristic_window    = 10;
        int    count_heuristic_threshold = 12;

        // Free-form Pi-style appendix to the layer-6 summary prompt. Empty by
        // default; users add things like "preserve all file paths and test
        // commands" here. Per-run /compact instructions override this for one
        // invocation only.
        std::string custom_summary_instructions;
    };

    // S5.I -- saved-session lifecycle + auto-cleanup. Sessions accumulate in
    // `.locus/sessions/` indefinitely; without cleanup a long-lived workspace
    // ends up with hundreds of stale JSONs. A session is a cleanup candidate
    // iff its `last_opened_at` is older than `delete_after_days` AND, by
    // ordering on `last_opened_at`, it sits beyond the `keep_last_count`
    // most-recently-opened CLOSED sessions. Currently-open tabs are always
    // exempt (pin = being-open-in-a-tab).
    struct Sessions {
        bool auto_cleanup_enabled = true;
        int  keep_last_count      = 10;
        int  delete_after_days    = 21;
        // S5.I -- when true, every open_tabs entry in workspace UI state is
        // re-opened on workspace open. When false a single empty tab opens.
        bool restore_last         = true;
        // When true, ActivityLog appends every emitted event to a sidecar
        // JSONL file (.locus/sessions/<id>.activity.jsonl) and replays the
        // file on load_session. Default off because (a) it doubles the
        // per-event I/O cost and (b) most users don't care about activity
        // history across restarts. Turn on from the Sessions settings tab
        // when investigating a multi-day session or saving a session as
        // QA evidence.
        bool persist_activity     = false;
    };

    // Sound alerts when the GUI needs the user's attention (tool approval,
    // ask_user question, agent turn complete, compaction-needed dialog). Each
    // event maps to a distinct Windows system sound alias (PlaySound + SND_ALIAS)
    // so the OS theme controls the actual waveform. `only_when_unfocused` gates
    // every sound on the main frame not being the foreground window -- avoids
    // chirping when the user is already watching the agent work.
    struct Notifications {
        bool sound_on_tool_approval  = true;
        bool sound_on_ask_user       = true;
        bool sound_on_turn_complete  = false;
        bool sound_on_compaction     = true;
        bool only_when_unfocused     = true;
    };

    // S5.A -- workspace capability buckets. Each bucket gates a family of
    // tools (and sometimes a system-prompt slot) so the per-turn manifest
    // only carries what the user asked for. `semantic_search` and
    // `memory_bank` are the canonical source of truth and propagate to the
    // older `index.semantic_search_enabled` / `memory.enabled` flags on load;
    // the legacy flags remain in the JSON for backward compat.
    //
    // Token estimates (used to label the UI checkboxes -- recomputed every
    // few milestones, not live-measured):
    //   background_processes  ~700 tokens (4 bg tools)
    //   semantic_search         ~80 tokens (search mode list pruning)
    //   code_aware_search      ~250 tokens (search mode list + outline tool)
    //   memory_bank            ~300 tokens + memory.in_context_budget_tokens
    //   web_retrieval          ~300 tokens (M6 -- placeholder)
    struct Capabilities {
        bool background_processes = false;
        bool semantic_search      = true;
        bool code_aware_search    = true;
        bool memory_bank          = true;
        bool web_retrieval        = false;
    };

    // S6.0 -- prompt-injection scanner + taint policy over UNTRUSTED external
    // ingress (web / ZIM / MCP). Workspace files are trusted and never scanned.
    // The scanner is a transparency tripwire, not a security boundary (the
    // approval gate is) -- see roadmap/M6/S6.0-prompt-injection-scanner.md.
    struct Security {
        // Scan web + MCP ingress for injection patterns on the way in.
        bool injection_scan = true;
        // Opt-in keyword scan over ZIM/Wikipedia content (S6.2). Off by default:
        // encyclopedia text is overwhelmingly benign and a multi-GB scan is not
        // worth it. The ZIM ORIGIN STAMP is unconditional regardless of this.
        bool scan_zim = false;
        // Findings at/above this confidence escalate the tool's approval to
        // `ask` (and Exfiltration escalates regardless of confidence).
        float block_confidence = 0.85f;
        // Cap on bytes inspected per ingress (head + tail windowing past it).
        int max_scan_kb = 256;
    };

    // Per-workspace tool approval overrides: tool_name -> policy.
    // Absent entries fall back to the tool's default (ITool::approval_policy()).
    // Kept at top level so the JSON shape ("tool_approvals" map at root) and
    // the C++ surface line up without a forced rename.
    std::unordered_map<std::string, ToolApprovalPolicy> tool_approval_policies;

    Index         index;
    Llm           llm;
    Git           git;
    Agent         agent;
    Memory        memory;
    Chat          chat;
    Compaction    compaction;
    Sessions      sessions;
    Notifications notifications;
    Capabilities  capabilities;
    Security      security;
};

// S6.17 Task H -- apply the `agent.prompt_cost` preset (if non-empty) onto
// `cfg.agent.lazy_tool_manifest` + `cfg.agent.system_prompt_profile` in
// place. Empty preset is a no-op so users can keep the individual flags.
// Callers: WorkspaceConfig load path (the JSON reader applies presets after
// loading so the rest of the codebase sees the resolved values without
// having to know about the preset). The mapping branches on context limit
// for the "balanced" default-by-context case described in the spec.
//   <= 16k context: balanced
//   > 64k context : verbose
// (Anything in between continues to honour the individual flags.)
void prompt_cost_apply(WorkspaceConfig& cfg);

// Pure helper used by the prompt-cost Settings panel preview and the test
// harness. Returns the {lazy, profile} pair a given preset name maps to.
// Unknown / empty preset returns {cfg.agent.lazy_tool_manifest, cfg.agent.system_prompt_profile}
// so callers can show "Custom" without recomputing.
struct PromptCostFlags {
    bool        lazy_tool_manifest;
    std::string system_prompt_profile;
};
PromptCostFlags prompt_cost_to_flags(const std::string& preset,
                                     int context_limit,
                                     bool fallback_lazy,
                                     const std::string& fallback_profile);

// S5.A token-cost estimates, used to label the capability checkboxes in
// both the first-open modal and the Settings -> Capabilities tab. Hand-
// measured against the M4 manifest (Dec 2026 baseline). Re-measure once a
// milestone if tool descriptions drift materially.
namespace capability_token_estimates {
    inline constexpr int k_background_processes = 700;
    inline constexpr int k_semantic_search      = 80;
    inline constexpr int k_code_aware_search    = 250;
    inline constexpr int k_memory_bank          = 300;
    inline constexpr int k_web_retrieval        = 300;
}

} // namespace locus

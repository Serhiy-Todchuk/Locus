#pragma once

#include "core/workspace_services.h"
#include "../tools/tool.h"  // for ToolApprovalPolicy

#include <filesystem>
#include <functional>
#include <string>
#include <memory>
#include <unordered_map>

namespace locus {

namespace fs = std::filesystem;

class Database;
class Embedder;
class EmbeddingWorker;
class ExtractorRegistry;
class FileWatcher;
class IndexQuery;
class Indexer;
class MemoryStore;
class ProcessRegistry;
class Reranker;
class WatcherPump;
class WorkspaceLock;

struct WorkspaceConfig {
    // Index settings
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

    // S4.L -- per-turn auto-commit. When `git_auto_commit` is true AND the
    // workspace contains a `.git/` directory AND at least one file mutation
    // happened this turn, AgentCore runs `git add -A && git commit -m
    // "<prefix><agent-summary>"` after the turn yields. Failures surface as a
    // single warning and don't block the agent; the user resolves manually.
    // `git_commit_branch` (default empty -> use current branch) lets users
    // park agent commits on a side branch; if the named branch doesn't exist,
    // it's created once and a warning is logged. `git_commit_prefix` is
    // prepended to the commit message body.
    bool        git_auto_commit   = false;
    std::string git_commit_branch = "";
    std::string git_commit_prefix = "[locus] ";

    // Semantic search (on by default - gracefully disabled if model missing).
    // Embedding dimension is derived from the loaded GGUF, never from config:
    // swapping models triggers an automatic re-embed on next workspace open.
    bool semantic_search_enabled = true;
    std::string embedding_model = "bge-m3-Q8_0.gguf";
    int chunk_size_lines = 80;
    int chunk_overlap_lines = 10;

    // Reranker (cross-encoder, S4.J). Off by default until the model is
    // present so first-run users without the GGUF aren't blocked. When on,
    // search_semantic / search_hybrid fetch reranker_top_k bi-encoder
    // candidates and rerank to the requested max_results.
    bool reranker_enabled = false;
    std::string reranker_model = "bge-reranker-v2-m3-Q8_0.gguf";
    // 20 candidates × ~100ms/rerank in Release on CPU = ~2s wall-clock per
    // search call, which is the practical ceiling before a synchronous
    // tool invocation feels broken. Bump only if you're on a workstation
    // CPU or move to GPU.
    int reranker_top_k = 20;

    // LLM settings (persisted per-workspace)
    std::string llm_endpoint = "http://127.0.0.1:1234";
    std::string llm_model;           // empty = server default
    double      llm_temperature = 0.7;
    int         llm_context_limit = 0; // 0 = auto-detect from server

    // Per-request completion cap. The 8192 default is sized to accommodate
    // a single big-file write_file payload plus a few hundred tokens of
    // reasoning -- 2048 (the old default) routinely truncated multi-tool-
    // call responses on coding tasks, leaving an empty-arguments tool call
    // that poisoned the next round (HTTP 500 from the chat template).
    // Bump higher if your model is happy with longer answers; the server
    // always clamps to its own ceiling regardless of what we send.
    int         llm_max_tokens = 8192;

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
    int         llm_timeout_ms = 1800000;

    // S4.N -- tool-call wire format. Stored as a string so a config.json
    // edited by the user reads naturally; parsed via tool_format_from_string.
    // "auto" runs OpenAI JSON tool_calls + Qwen/Claude XML extraction in
    // parallel, which is the right default for unknown models. Override to
    // "openai" / "qwen" / "claude" / "none" for a known model family.
    std::string llm_tool_format = "auto";

    // S4.V Task 7 -- power-user samplers. Defaults of 0 mean "don't include
    // in the request"; the server's per-model defaults remain in effect.
    // Field names match llama.cpp / LM Studio's accepted JSON keys.
    double      llm_top_p          = 0.0;
    int         llm_top_k          = 0;
    double      llm_min_p          = 0.0;
    double      llm_repeat_penalty = 0.0;
    // OpenAI-protocol penalties. Range [-2, 2]; 0.0 sentinel = "don't send".
    // Distinct from `llm_repeat_penalty` (llama.cpp's multiplicative dial);
    // the three compose rather than overlap.
    double      llm_frequency_penalty = 0.0;
    double      llm_presence_penalty  = 0.0;

    // Per-workspace tool approval overrides: tool_name -> policy.
    // Absent entries fall back to the tool's default (ITool::approval_policy()).
    std::unordered_map<std::string, ToolApprovalPolicy> tool_approval_policies;

    // S3.L -- token-cost guardrail. AgentCore logs the per-turn tool-manifest
    // size at info level every turn; emits a warning when the manifest crosses
    // this threshold. 4000 ≈ 12% of a 32K context -- a reasonable "we've grown
    // too much" signal once M4 adds ~20 more tools.
    int tool_manifest_warn_tokens = 4000;

    // S4.I -- per-background-process output ring buffer cap. The reader thread
    // appends stdout+stderr until this many bytes are buffered; older bytes
    // are dropped from the front (the LLM is told how many it missed). 256 KB
    // covers a verbose dev-server log between turns without paging the agent
    // thread, and is still cheap (a few processes × 256 KB is negligible).
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

    // S4.R -- workspace-scoped memory bank. When enabled, two tools land in the
    // manifest (`add_memory`, `search_memory`), a slot is reserved in the
    // system prompt for pinned + recently-used entries, and `/memorize` is
    // accepted as a slash command. Disable to remove all three surfaces.
    bool memory_enabled                    = true;
    // Token cap on the always-in-context memory slot. All `pinned:true`
    // entries are injected first; the remaining budget is filled with the
    // most-recently-used unpinned entries. Anything dropped from the slot
    // remains searchable via `search_memory`.
    int  memory_in_context_budget_tokens   = 500;
    // GC ceiling: unpinned entries beyond this count are pruned (oldest by
    // creation time first) on workspace open and after every `add_memory`.
    // Pinned entries are never GC'd.
    int  memory_max_entries                = 200;
    // Per-call cap on the bytes a single `search_memory` response can return
    // to the LLM. Caps the total content size, not the count -- a single
    // very long entry can still appear at the top of the response.
    int  memory_search_response_max_tokens = 1500;
    // Recency half-life in days for the soft recency factor applied during
    // hybrid memory search. `0` disables the recency contribution (rely on
    // BM25 + semantic + reranker only). The default of 21 days matches the
    // stage doc: a 21-day-old entry contributes half as much as a fresh one,
    // smoothly decaying. Justified for the memory corpus (preferences /
    // conventions genuinely supersede over time) but deliberately not used
    // for workspace search.
    int  memory_recency_half_life_days     = 21;

    // S5.D -- minimum response headroom the agent loop guarantees the LLM.
    // Negative value = auto: std::min(context_limit / 5, 4096).
    // When context_limit is 0 (auto-detect not yet done), reserve is 0.
    int  compaction_reserve_tokens   = -1;

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
        bool   auto_enabled    = true;
        double warn_threshold  = 0.70;
        double auto_threshold  = 0.85;

        // Which cascade layers auto-compact runs (and the initial state of
        // the per-layer checkboxes in the /compact dialog). Persisted via
        // the dialog's Save button so the user can tailor the automatic
        // cascade without editing config.json. Defaults match the original
        // hardcoded auto cascade (1+2+3+6).
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

        // Free-form Pi-style appendix to the layer-6 summary prompt. Empty by
        // default; users add things like "preserve all file paths and test
        // commands" here. Per-run /compact instructions override this for one
        // invocation only.
        std::string custom_summary_instructions;
    };
    Compaction compaction;

    // S5.D -- per-message token chip in chat. When true, each bubble shows a
    // small grey "(N t)" estimate. Off-switch for users who find it noisy.
    bool ui_show_per_message_tokens  = true;

    // S5.C -- inline diff rendering in chat. When `chat_show_diffs` is true,
    // successful `edit_file` / `write_file` / `delete_file` calls render a
    // red/green unified diff in the chat history below the tool bubble.
    // `chat_diff_max_lines` caps the number of diff lines rendered per call
    // before a "(N more lines collapsed)" marker is emitted -- guards against
    // a single huge file write blowing up the chat HTML.
    bool chat_show_diffs    = true;
    int  chat_diff_max_lines = 200;
    // Lines of unchanged surrounding context shown before and after each
    // change inside an inline diff. 0 collapses everything to just the
    // add/del lines (the pre-S5.Z layout). Default 4 matches `diff -u`.
    int  chat_diff_context_lines = 4;
    // S5.Z #2 -- soft-collapse threshold for write_file diffs. Diffs longer
    // than this many rows render the first N rows inline and fold the rest
    // into a `<details>`/`<summary>` (native HTML toggle, no JS). 0 disables
    // collapsing -- show every row inline.
    int  chat_diff_collapse_threshold = 16;

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
    };
    Sessions sessions;

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
    Notifications notifications;

    // S5.A -- workspace capability buckets. Each bucket gates a family of
    // tools (and sometimes a system-prompt slot) so the per-turn manifest
    // only carries what the user asked for. `semantic_search` and
    // `memory_bank` are the canonical source of truth and propagate to the
    // older `semantic_search_enabled` / `memory_enabled` flags on load; the
    // legacy flags remain in the JSON for backward compat.
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
    Capabilities capabilities;
};

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

// Owns all workspace-level resources: config, database, file watcher, LOCUS.md.
// One Workspace instance per open folder. Implements `IWorkspaceServices` so it
// can be passed directly to `AgentCore` and tools -- no adapter wrapper needed.
class Workspace : public IWorkspaceServices {
public:
    // Opens a workspace at the given path. Creates .locus/ if absent.
    // Throws std::runtime_error on failure.
    explicit Workspace(const fs::path& root);
    ~Workspace() override;

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // -- IWorkspaceServices ---------------------------------------------------
    const fs::path&    root() const override     { return root_; }
    IndexQuery*        index() override          { return query_.get(); }
    EmbeddingWorker*   embedder() override       { return embedding_worker_.get(); }
    Reranker*          reranker() override       { return reranker_.get(); }
    ProcessRegistry*   processes() override      { return processes_.get(); }
    ProcessSinkBroker* process_sink() override;
    MemoryStore*       memory() override         { return memory_.get(); }
    Workspace*         workspace() override      { return this; }

    // -- Workspace-specific ---------------------------------------------------
    const fs::path& locus_dir() const { return locus_dir_; }

    const WorkspaceConfig& config() const { return config_; }
    WorkspaceConfig& config() { return config_; }
    void save_config();

    // LOCUS.md content (empty string if file doesn't exist). Read-only.
    const std::string& locus_md() const { return locus_md_; }

    Database& database() { return *main_db_; }
    // Semantic DB (chunks + chunk_vectors). Null when semantic search disabled.
    Database* vectors_database() { return vectors_db_.get(); }
    ExtractorRegistry& extractors() { return *extractors_; }
    FileWatcher& file_watcher() { return *watcher_; }
    Indexer& indexer() { return *indexer_; }
    IndexQuery& query() { return *query_; }
    WatcherPump& watcher_pump() { return *watcher_pump_; }

    EmbeddingWorker* embedding_worker() { return embedding_worker_.get(); }

    // Hot-toggle semantic search at runtime (creates/destroys embedder + worker).
    // Returns true on success, false if model not found or init failed.
    bool enable_semantic_search();
    void disable_semantic_search();

    // Process-wide override hook. When set, enable_semantic_search() takes the
    // returned embedder instead of loading a GGUF from disk. The model id
    // recorded in vectors.db comes from the returned embedder's filename, so
    // schema migration sees a stable identity. Designed for tests that share
    // a single small embedder across many Workspace instances. Pass nullptr
    // to revert to the default disk-load path.
    using EmbedderProvider = std::function<std::shared_ptr<Embedder>()>;
    static void set_embedder_provider(EmbedderProvider p);

private:
    void load_config();
    void load_locus_md();

    fs::path root_;
    fs::path locus_dir_;
    WorkspaceConfig config_;
    std::string locus_md_;
    // Held first, released last -- stops another Locus process from opening
    // this workspace (or a nested/ancestor one) while we own it.
    std::unique_ptr<WorkspaceLock> lock_;
    std::unique_ptr<Database> main_db_;
    std::unique_ptr<Database> vectors_db_;  // null when semantic disabled
    std::unique_ptr<ExtractorRegistry> extractors_;
    std::unique_ptr<FileWatcher> watcher_;
    std::unique_ptr<Indexer> indexer_;
    std::unique_ptr<IndexQuery> query_;
    // shared_ptr so a process-wide hook (`set_embedder_provider`) can hand the
    // same Embedder to multiple Workspace instances -- primarily a test-suite
    // optimisation; production unique-Workspace use is unaffected.
    std::shared_ptr<Embedder> embedder_;
    std::unique_ptr<EmbeddingWorker> embedding_worker_;
    std::unique_ptr<Reranker> reranker_;
    // Owned after `indexer_` so it stops + joins before the indexer is torn
    // down (the pump's background thread feeds events into the indexer).
    std::unique_ptr<WatcherPump> watcher_pump_;
    // S4.I -- long-running shell processes spawned by the agent. The dtor
    // terminates every still-running child so workspace close never leaks
    // background processes.
    std::unique_ptr<ProcessRegistry> processes_;
    // S4.R -- workspace-scoped memory bank. Optional: null when
    // `WorkspaceConfig::memory_enabled` is false. Reuses the workspace
    // embedder and reranker via raw pointers stored at construction time;
    // hot-toggling those isn't supported (re-open the workspace).
    std::unique_ptr<MemoryStore> memory_;
};

} // namespace locus

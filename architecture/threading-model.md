# Threading Model

The authoritative map of threads, shared state, and cross-thread invariants in Locus Core.
Every M4/M5 feature that spawns a thread (LSP clients, MCP servers, parallel tool dispatch,
background commands, subagents) must preserve the invariants in §3 and follow the checklist
in §4.

For the turn-level orchestration see [agent-loop.md](agent-loop.md); for data flow see
[overview.md](overview.md). This doc only covers threads and synchronization.

---

## 1. Thread inventory

Five long-lived threads today. Each row lists what the thread reads and writes, and how it
starts / stops.

| Thread | Owner (lifetime) | Reads | Writes | Start / stop |
|---|---|---|---|---|
| **Main / UI thread** | process entry | everything (owns `AgentCore`, `Workspace`, frontends) | invokes `ILocusCore` methods; forwards tool decisions | spawned by OS at `main` / `WinMain`; exits last |
| **Agent thread** | `AgentCore::agent_thread_` | `message_queue_`, `history_`, `llm_`, `tools_`, `services_`, `activity_`, `budget_`, `frontends_` | `history_` (exclusive during a turn), `activity_.buffer_`, `budget_` counters, `busy_` / `cancel_requested_` | `AgentCore::start()` → `std::thread` wrapping `agent_thread_func()`; joined in `AgentCore::stop()` after `running_=false` + `queue_cv_.notify_one()` + `dispatcher_->wake()` |
| **WatcherPump thread** | `WatcherPump::thread_` ([src/core/watcher_pump.cpp](../src/core/watcher_pump.cpp)) | `FileWatcher::pending_` (via `drain`) | `Indexer` prepared statements (via `process_events`), main DB, vectors DB | `WatcherPump::start()` during `Workspace` ctor; joined in `stop()` (via dtor) with `stopping_=true` + `cv_.notify_all()` |
| **Embedding worker thread** | `EmbeddingWorker::thread_` ([src/embedding_worker.cpp](../src/embedding_worker.cpp)) | its own SQLite prepared statements against the vectors DB, `Embedder` (CPU llama.cpp session), `pending_ids_` | `chunk_vectors` rows, `done_`/`total_` counters | `EmbeddingWorker::start()` during `Workspace` ctor (when semantic enabled); joined in `stop()` via `running_.exchange(false)` + `queue_cv_.notify_all()` |
| **efsw internal watch thread(s)** | `efsw::FileWatcher` (owned by `locus::FileWatcher::watcher_`) | OS change notifications | `FileWatcher::pending_` via `push_raw` (callback path) | starts on `FileWatcher::start()` → `watcher_->watch()`; stops when `efsw::FileWatcher` destructs |

**Caller threads that never form a dedicated loop but touch core state:**

- The WebSocket server thread pool (M5, not yet landed) will call `ILocusCore` methods from
  Crow handler threads.
- `cpr` runs its libcurl SSE reads on the agent thread (no extra thread is spawned for
  streaming); callbacks fire inline on whichever thread invoked `stream_completion`, which is
  always the agent thread in `AgentLoop::run_step`.

### Future threads (M4/M5 additions — must follow §3)

- **LSP reader(s)** (S4.E): one `std::thread` per language server reading JSON-RPC framed
  messages from stdout.
- **MCP reader(s)** (S4.G): one thread per MCP server on stdio.
- **Parallel tool workers** (S4.H): thread pool sized by `std::thread::hardware_concurrency`
  for read-only tools marked parallel-safe.
- **Background command threads** (S4.I): one per long-running `run_command` invocation.
- **Subagent threads** (S4.U): a child `AgentCore` runs on its own thread; the parent
  orchestrates via a scoped channel.

---

## 2. Shared resources and what protects them

| Resource | Protector | Notes |
|---|---|---|
| `AgentCore::message_queue_` | `queue_mutex_` + `queue_cv_` | Producers (any thread) push, the agent thread pops. `queue_cv_.notify_one()` on push; waiter predicate checks `!running_` for shutdown. |
| `AgentCore::sync_turn_done_` | `sync_mutex_` + `sync_cv_` | One-shot completion signal for `send_message_sync` (CLI). The agent thread sets and notifies after each turn. |
| `AgentCore::attached_context_` | `attached_mutex_` | Read on the agent thread (via `compose_system_prompt`) and the UI thread (`attached_context()`). Written from the thread invoking `set_attached_context` / `clear_attached_context`. |
| `AgentCore::busy_`, `cancel_requested_`, `running_` | `std::atomic<bool>` | Relaxed-ordering flags. `cancel_requested_` is passed by reference to `ToolDispatcher` as its sole cancel signal. |
| **`ConversationHistory::messages_`** (inside `AgentCore::history_`) | **single-writer invariant** + debug fence (see §3.1) | Not a mutex — correctness rests on the invariant that only the agent thread mutates it during a turn. `ConversationHistory::assert_owner_thread()` catches violations (spdlog::error + debug abort). |
| `ToolDispatcher::pending_decision_`, `pending_modified_args_` | `decision_mutex_` + `decision_cv_` | Producer is any thread calling `AgentCore::tool_decision`. Consumer is the agent thread inside `ToolDispatcher::dispatch`. `cancel_flag_` (shared reference to `AgentCore::cancel_requested_`) breaks the wait. |
| `ActivityLog::buffer_`, `next_id_` | `mutex_` (internal) | Events emitted from any thread (`emit_index_event` is called from the indexer via callback). Broadcast is done outside the lock. |
| `FrontendRegistry::frontends_` | `mutex_` (internal) | Registration and broadcast both take the mutex. `broadcast()` catches per-frontend exceptions so one frontend throwing does not starve others. |
| `FileWatcher::pending_` | `mutex_` (internal) | Producer is the efsw callback thread via `push_raw`. Consumer is `WatcherPump` on its own thread via `drain`. |
| `WatcherPump::pending_`, group timestamps | `mutex_` + `cv_` | Background thread drains + batches; `flush_now()` (any thread) takes the same mutex. `flush_locked` releases the mutex while `Indexer::process_events` runs so a long batch does not block `flush_now`. |
| `Indexer::process_mutex_` | mutex | Serialises concurrent `process_events` calls (pump thread vs. the synchronous `flush_now` caller). Prepared statements are not safe under concurrent use. |
| `EmbeddingWorker::pending_ids_` | `queue_mutex_` + `queue_cv_` | Producer (Indexer, agent thread) `enqueue`; consumer (worker thread) pops. `total_` / `done_` are `std::atomic<int>`. |
| `EmbeddingWorker`'s SQLite prepared statements | thread-affinity | Prepared on and used only by the worker thread (see `thread_func`). A separate SQLite connection per thread is the WAL-friendly invariant that lets the embedder run concurrently with the indexer. |
| SQLite main DB connection (`Database::handle_`) | thread-affinity + WAL | The main connection is written only by the agent thread (tool-driven edits) and by the indexer (via `WatcherPump`). WAL mode lets readers (e.g. `IndexQuery` on the agent thread) proceed without blocking writers. Each long-lived writer keeps its own set of prepared statements. |
| SQLite vectors DB connection | thread-affinity + WAL | Same pattern as main DB. Indexer chunks + embedding worker both write, but on separate `Database` instances — no connection sharing. |

---

## 3. Invariants

These are the rules that must continue to hold as new threads arrive in M4/M5. Each is paired
with the mechanism that enforces it today.

### 3.1 The agent thread is the only writer of `ConversationHistory` during a turn

`ConversationHistory` is not mutex-protected. Correctness comes from restricting writers to
the agent thread while a turn is in flight. Enforcement:

- `AgentCore::agent_thread_func` wraps each call to `process_message` in a
  `ConversationOwnerScope` RAII guard which sets `history_.owner_thread_id_` to the agent
  thread on entry and clears it on exit.
- `ConversationHistory::add` / `clear` / `drop_tool_results` / `drop_oldest_turns` /
  `replace_system_prompt` all call `assert_owner_thread()`. If an owner is set and the caller
  is not it, the fence logs a `spdlog::error` and `assert(false)` in debug builds.
- `AgentLoop::run_step` returns `AgentStepResult` by value; it never calls `history_` mutators
  directly.
- `ToolDispatcher::dispatch` takes an `AppendFn` callback (a lambda that calls
  `history_.add`) and runs the callback on the agent thread.

Between turns the owner is intentionally unset: inter-turn operations (CLI REPL `/reset`,
`/compact`, `/load`, and the GUI equivalents) currently mutate `history_` from the calling
thread while the agent thread is idle on `queue_cv_`. This is safe today because those
entry points are only reached when the agent thread is blocked. If M4 adds a path that could
call them while `busy_` is true (e.g. cancel-and-reset, subagent delegation), that caller
must route the mutation through the agent thread or the fence will fire.

### 3.2 Frontend callbacks fire on the agent thread

Every `IFrontend::on_*` method (`on_token`, `on_tool_call_pending`, `on_activity`, …) is
invoked on the agent thread via `FrontendRegistry::broadcast`. Frontends that live on a
different thread (wxWidgets) must marshal to their UI thread themselves — see
`WxFrontend::on_token` forwarding a `wxThreadEvent` via `wxQueueEvent`. The CLI frontend is
already on the agent thread's synchronous wait (`send_message_sync`) so no marshaling is
needed.

The one exception is frontends registered by late-joining clients (websocket, future remote
frontends): their own adapter must dispatch onto a send queue.

### 3.3 `tool_decision()` and `cancel_turn()` are thread-agnostic

Users click approve / reject / cancel on whatever thread owns their UI. `AgentCore`
forwards both entry points to `ToolDispatcher` (and `cancel_requested_` respectively), which
use `decision_mutex_` + `decision_cv_` + the shared atomic cancel flag. Any new frontend
(websocket, VS Code shim) may call these without additional synchronization.

### 3.4 SQLite writers serialise through a single connection

Each SQLite database (`main.db`, `vectors.db`) is touched by at most two writers in steady
state: the agent thread (via tool-driven mutations) and the indexer (via `WatcherPump`).
Both go through the same `Database` handle, serialised by SQLite's internal mutex. WAL mode
lets readers from any thread (`IndexQuery`, GUI file tree) proceed without blocking writers.

The embedding worker is the exception: it opens its **own** `Database` on the vectors DB so
its write burst does not contend with the indexer's chunk-row writes. The split is
intentional — vectors are a separate DB file precisely so the two can WAL-commit in parallel.

Prepared statements are **not** thread-safe under concurrent use; `Indexer::process_mutex_`
serialises `process_events` callers (pump background thread vs. `flush_now` from main).

### 3.5 Approval waits are cancellable

`ToolDispatcher::dispatch` waits on `decision_cv_` with a predicate that includes
`cancel_flag_.load()`. Every new wait path added in M4 (plan-mode confirmation, parallel
dispatch coordination, MCP handshake) must follow the same pattern: the predicate reads the
shared cancel flag, and the cancel path calls the waiter's `wake()` to trip the condvar.
Otherwise `AgentCore::cancel_turn` hangs.

### 3.6 External-subsystem activity goes through `ActivityLog`

The indexer's `on_activity` callback and the embedding worker's `on_progress` callback fire
on their own threads. They must only touch thread-safe surfaces — `ActivityLog::emit_*` and
`FrontendRegistry::broadcast` both hold internal mutexes, so calling either is safe. Writing
to `history_` or `budget_` from these threads would violate §3.1.

---

## 4. Adding a new thread — checklist

When M4 or M5 introduces a thread, work through this list before merging:

1. **Decide its write set.** List every shared resource the thread mutates and pick a
   protector from §2, or add a new one with a documented invariant. If the answer is
   "`history_`", you are wrong — route through the agent thread via the existing message
   queue (or a future control queue) so §3.1 holds.
2. **Add a row to the §1 inventory table.** Give the thread an owner (a class member, not a
   detached thread) and describe its start/stop path.
3. **Confirm the stop path is clean.** The owner must have a `stop()` that (a) sets an atomic
   `running_=false`, (b) wakes whatever condvar the thread sleeps on, and (c) joins. Destructors
   call `stop()`. Worker threads must not outlive the objects they reference.
4. **Respect SQLite connection affinity.** If the thread writes a DB, give it its own
   `Database` instance or fully synchronise through an existing one's prepared statements.
5. **If it invokes `IFrontend` callbacks, document on which thread they fire.** Most
   callers should funnel through `ActivityLog` / `FrontendRegistry` on the agent thread; new
   direct callers must either marshal to the agent thread or be explicitly added to the
   §3.2 exception list.
6. **If it has a cancellable wait, include the shared cancel flag in the predicate** (see
   §3.5). `AgentCore::cancel_requested_` is already passed by reference to `ToolDispatcher`
   — follow the same shape.
7. **Update this doc.** A new thread without a row in §1 is a latent incident report.

---

## 5. Known current caveats

Captured here so future readers don't assume they're bugs or rediscover them the hard way.

- **Inter-turn mutation of `history_` from foreign threads is relied upon.**
  `AgentCore::reset_conversation` / `compact_context` / `load_session` /
  `set_attached_context` / `clear_attached_context` are invoked from the CLI REPL (main
  thread) and GUI menu (UI thread) while the agent thread is idle on `queue_cv_`. §3.1's
  owner fence is deliberately cleared between turns so these paths do not trip — but they
  are only race-free because `busy_` is false at the time of the call. A future "cancel and
  reset" flow or a subagent that issues these must first route the mutation through the
  agent thread.
- **`cpr` / libcurl does not spawn a thread for SSE.** Streaming callbacks fire on the agent
  thread synchronously during `stream_completion`. If a future backend moves to an async
  client, every `AgentLoop` callback will need to re-establish agent-thread affinity before
  touching `history_`.
- **efsw's internal thread count is platform-dependent.** On Windows it is one
  `ReadDirectoryChangesW` loop; on Linux it is one `inotify` reader. All of them funnel
  through `FileWatcher::push_raw`, which is the only place we observe them.
- **The embedder holds a single `Embedder` instance.** llama.cpp's session is not re-entrant
  — `embed_query` (called from the agent thread via tools) and the embedding worker's own
  `embedder_.embed` both take the same CPU session. Today both are serialised by the
  sequential nature of tool dispatch and the single worker thread; S4.H (parallel tool
  dispatch) must pool or mutex the embedder if it lets semantic-search tools run
  concurrently.

---

## 6. Where to look next

- [src/agent/agent_core.cpp](../src/agent/agent_core.cpp) — agent thread + queue + ownership guard
- [src/agent/conversation.cpp](../src/agent/conversation.cpp) — `assert_owner_thread()` fence
- [src/agent/tool_dispatcher.cpp](../src/agent/tool_dispatcher.cpp) — approval condvar, cancel flag
- [src/core/watcher_pump.cpp](../src/core/watcher_pump.cpp) — background drain thread
- [src/embedding_worker.cpp](../src/embedding_worker.cpp) — worker thread, own DB connection
- [src/file_watcher.cpp](../src/file_watcher.cpp) — efsw listener bridge
- [src/frontend_registry.h](../src/frontend_registry.h) — fan-out + per-frontend exception isolation
- [agent-loop.md](agent-loop.md) §8 — turn-level invariants that intersect with §3 above

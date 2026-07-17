# 2026-07-06 Threading / Lifetime / Cancellation Audit

Deep adversarial pass over the seams where latent bugs hide from per-stage testing:
worker-thread teardown, MCP lifecycle, GUI<->agent-thread bridge, transport retry,
tab close, compaction. Every finding was verified against the code (not pattern-matched);
file:line refs are as of commit 6924aad. Findings are ranked; fix the CRITICAL band first.
The "Load-bearing guards" section at the bottom is the inverse output: correctness-critical
code a future refactor must not "simplify" away.

None of these are reachable from the happy path a single-tab, local-LM-Studio,
never-restart-MCP session exercises -- which is why the suites are green. They live on
the paths multi-tab + hosted-endpoint + MCP + macOS usage opens up.

---

## CRITICAL

### A1. `McpManager::restart()` -- UAF on the old client + join-under-mutex deadlock
`src/mcp/mcp_manager.cpp:166-177`, `src/mcp/mcp_tool.cpp:59-141`, `src/mcp/stdio_transport.cpp:40`

`restart()` (reachable from Settings > MCP, GUI thread) does
`e->client->stop(); e->client = std::make_unique<McpClient>(...); start_one_locked(*e);`.
Three defects:

- **(a)** The old `McpClient` is deleted at the assignment, but the registry's `McpTool`
  entries (raw `client_` pointers) are unregistered only inside `register_tools_locked`,
  which runs only after the NEW client's `start()` succeeds. During startup (seconds) the
  agent thread's per-round `build_schema_json` calls `McpTool::available()` ->
  `client_->status()` on freed memory. If the restarted server FAILS to start,
  `start_one_locked` returns early and the dangling tools stay registered forever.
- **(b)** `restart()` on the GUI thread while the agent thread sits in
  `McpClient::send_request_sync`: `stop()` wakes the waiter but nothing fences it out of
  the object before the assignment deletes it.
- **(c)** `ToolRegistry` has no mutex (`tool_registry.h:27-30`); restart mutates it from
  the GUI thread while the agent thread iterates it mid-turn.
- **Deadlock variant**: `on_crash` runs on the transport reader thread and takes `mu_`
  (`mcp_manager.cpp:81-84`). If the server crashes right as the user clicks Restart,
  `restart()` holds `mu_` and destroys the client -> `~StdioTransport` -> `reader_.join()`
  on a thread blocked on `mu_`. Permanent GUI freeze. `stop_all()` avoids exactly this
  (swap out under `mu_`, destroy outside); `restart()` never adopted the pattern.

Fix shape: make restart follow `stop_all()`'s swap-out-then-destroy-outside-lock pattern,
unregister the old tools BEFORE destroying the old client, and give `ToolRegistry` a mutex
(or confine all mutation to the agent thread via the pending-control-request queue).

### A2. Transport mid-stream stall retry duplicates / corrupts already-streamed output
`src/llm/openai_transport.cpp:278-308`, `src/llm/llm_client.cpp:554,627-644`, `src/agent/agent_loop.cpp:373-377`

`classify_retry` retries `OPERATION_TIMEDOUT` regardless of `chunks_received`. A stream
that delivered N chunks and then stalled is re-POSTed with the SAME callbacks:
`decoder_->reset()` runs once per send (before `post_chat`), and `AgentLoop`'s
`accumulated_text += token` never resets -- so attempt 2's fresh completion concatenates
onto attempt 1's partial text (user-visible duplicated prose), and partial tool-call
deltas from two different completions merge in `tool_accum` (malformed args). The XML
extractors' held-back partial-tag bytes also survive into the retry.

Fix shape: either (cheapest) only retry a timeout when `chunks == 0` for the attempt --
matching the empty-200 rule -- or add an `on_retry_attempt` callback that lets
`LMStudioClient` reset the decoder and lets `AgentLoop` discard the partial accumulation
(harder: tokens already streamed to the UI need a bubble reset).

### A3. Indexer / EmbeddingWorker callback slots: unsynchronized reassignment + dangling-agent windows
`src/core/locus_session.cpp:243-251,277-286,316-325,335-360`, `src/core/workspace.cpp:437-453`, `src/index/indexer.cpp:781,820,896`, `src/index/embedding_worker.cpp:55,200,226`

One bug family, four expressions:

- **(a) Torn `std::function`.** `indexer().on_activity` / `embedding_worker()->on_activity`
  are plain `std::function` members invoked on the pump/embedding threads
  (`if (on_activity) on_activity(...)`) and reassigned on the GUI thread on EVERY tab
  switch (`set_active_tab`), tab add, and in both dtors' invariant-mandated nulling.
  Unsynchronized write-vs-invoke is UB; the check-then-call is also a TOCTOU.
- **(b) `close_tab` / `delete_tab` erase before re-point.** `tabs_.erase(...)` destroys the
  AgentCore the callbacks capture by raw pointer; the re-point happens only afterwards in
  `set_active_tab(active_index_)`. Closing the ACTIVE tab (the common case) leaves the
  callbacks dangling across the whole erase -- which joins a mid-turn agent thread and
  kills that tab's bg processes (seconds). A pump flush or embedding batch in that window
  calls `emit_index_event` on the freed agent. `~LocusSession` honors the null-first rule;
  `close_tab`/`delete_tab` do not.
- **(c) `disable_semantic_search()` order.** It stops/resets the worker FIRST and nulls
  `indexer_->on_chunks_created` LAST (`workspace.cpp:437-453`) -- the pump thread can call
  `embedding_worker_->enqueue()` through the stale callback on a null unique_ptr in
  between. Null the callback first, fence the pump (`flush_now`), then reset the worker.
- **(d) `MemoryStore` keeps raw `EmbeddingWorker*` / `Reranker*`** captured at construction;
  `disable_semantic_search()` frees both. Any memory search after a runtime semantic-off
  toggle dereferences freed objects.

Fix shape: give `Indexer` and `EmbeddingWorker` a small callback mutex (invoke under it,
assign under it) or a `std::shared_ptr<std::function>` atomic swap; in `close_tab` /
`delete_tab`, null or re-point the callbacks BEFORE `tabs_.erase`; reorder
`disable_semantic_search`; null MemoryStore's pointers there too.

### A4. MetricsView 1s timer polls a freed MetricsAggregator after its tab closes
`src/frontends/gui/locus_frame.cpp:412`, `src/frontends/gui/activity_panel.cpp:97-99`, `src/frontends/gui/metrics_view.cpp:112-132`

The workspace-shared ActivityPanel binds `MetricsView` to the CONSTRUCTION-TIME active
tab's `AgentCore::metrics()` by reference. Open a second tab, close the first: the
`LocusTab` -> `AgentCore` -> `MetricsAggregator` chain is destroyed, and within <=1 s the
wxTimer locks the destroyed mutex / reads freed vectors. Nothing re-points
`ActivityPanel::core_` / `MetricsView::metrics_` on tab close. The CROSS-WORKSPACE
instance of this exact race is known and guarded (`locus_app.cpp:349-354`,
`DeletePendingObjects()` before `session_.reset()`); the in-frame tab-close instance has
no guard. Fix shape: re-point (or stop the timer + grey the view) in `teardown_tab_ui`,
mirroring how the terminal panel does `set_state(nullptr)`.

### A5. `EmbeddingWorker::stop()` lost wakeup -> rare permanent hang at workspace close
`src/index/embedding_worker.cpp:29-35` vs `:111-114`

`stop()` flips `running_` and calls `notify_all()` WITHOUT holding `queue_mutex_`. If the
worker has evaluated the wait predicate (false) but not yet blocked, the notify is lost
and the worker sleeps forever; `thread_.join()` then hangs `Workspace::~`. Nanosecond
window, so it presents as a rare unkillable-process-on-exit. `WatcherPump::stop()`
(`watcher_pump.cpp:34-38`) does it correctly -- set the flag under the mutex, then notify.
Copy that discipline (an empty `lock_guard` scope between the exchange and the notify
suffices).

---

## HIGH

### B1. Use-after-move disables the S6.21 unverified-success tripwire
`src/agent/agent_turn.cpp:266` vs `:305`

`core_.ctx_->add_message(std::move(step.assistant_msg))` runs before
`check_unverified_success(step.assistant_msg.content, {})` -- the guard always reads a
moved-from (empty) string and can never fire in the live path. The pure predicate's unit
tests pass; the wiring is dead. Capture the content (or just a `std::string_view` copy of
it) before the move.

### B2. No SIGPIPE handling on POSIX -- dead child kills the whole app
`src/tools/process_registry.cpp:248-265` (write_stdin), `src/mcp/stdio_transport.cpp` (send_line), no `SIGPIPE`/`F_SETNOSIGPIPE` anywhere in `src/`

On macOS, writing to a pipe whose reader died raises SIGPIPE; default action terminates
the process. Both the terminal panel's stdin forward and MCP's `send_line` hit this the
moment a child/server dies between the `status_ == running` check and the `::write`.
Fix: `signal(SIGPIPE, SIG_IGN)` at startup (both CLI + GUI) or `fcntl(fd, F_SETNOSIGPIPE, 1)`
on every parent-side write fd, then handle `EPIPE`.

### B3. POSIX `StdioTransport::terminate()` can deadlock behind a blocked `send_line`
`src/mcp/stdio_transport.cpp:580-593` vs `:564-577`

`terminate()` takes `write_mu_` to close stdin BEFORE sending `killpg`. If the agent
thread's `send_line` is blocked in `::write` on a full pipe (wedged server, >64 KB args),
it holds `write_mu_` forever, so `killpg` is never reached and nothing unblocks the write.
Windows does it right (`TerminateJobObject` first, no `write_mu_` -- the broken pipe fails
the blocked `WriteFile`). Move `killpg(SIGTERM)` before the mutex-guarded close.

### B4. wxAuiNotebook tab drag breaks the notebook-index == session-index assumption
`src/frontends/gui/locus_frame.cpp:404-406,852-854,889,917-919,969-971,1001-1003`

The notebook is created with `wxAUI_NB_DEFAULT_STYLE` (includes `TAB_MOVE` + `TAB_SPLIT`);
there is no drag-done handler and `LocusSession` has no reorder API. After a drag, every
`teardown_tab_ui(sel); session_.close_tab(sel);` pair tears down UI for one tab and
destroys a DIFFERENT `LocusTab`, leaving `TabUi::tab` dangling for the router's next
event. Cheapest fix: strip `TAB_MOVE|TAB_SPLIT` from the style. Right fix: resolve session
tabs by tab_id, not index.

---

## MEDIUM

- **C1. Retry backoff sleep is not cancel-interruptible** (`openai_transport.cpp:300`).
  Up to 60 s (`Retry-After` capped at `max_backoff_ms*3`) of uninterruptible
  `sleep_for` on the agent thread; Stop / tab close / app quit joins block for the full
  backoff. Replace with a condvar wait or a chunked sleep polling `should_cancel`.
- **C2. `on_agent_compaction` is not tab-routed** (`agent_event_router.cpp:184-190`,
  `locus_frame.cpp:1131-1161`). A background tab crossing its threshold pops the dialog
  against -- and compacts -- the ACTIVE tab's conversation.
- **C3. `stop_all()` mutates `ServerEntry` outside `mu_` while a late `crash_cb_` reads it
  under `mu_`** (`mcp_manager.cpp:143-164` vs `:81-84`): concurrent read/clear of
  `namespaced_tool_names`. Narrow window.
- **C4. `FileWatcher::push_raw` Moved branch uses the THROWING `fs::relative`**
  (`file_watcher.cpp:147` vs the guarded `:110-119`): an AV/lock-race throw drops the
  whole rename event instead of taking the lexical fallback; also does fs I/O under the
  debounce mutex.
- **C5. `EmbeddingWorker` post-stop drops the queue and skips the shutdown WAL checkpoint**
  (`embedding_worker.cpp:107,115-119`): the A.6 drain checkpoint only runs on the
  `!running_ && empty` break; `stop()` with a non-empty queue processes one more chunk,
  drops the rest, no `wal_checkpoint_passive`. Behavior gap, not a crash.
- **C6. Tool-runtime watchdog can swallow a concurrent user cancel**
  (`tool_dispatcher.cpp:495-496`): when the guardrail fired, `cancel_flag_` is
  unconditionally cleared -- a user Stop that landed during the same tool execution is
  lost and the turn continues.

## LOW / LATENT

- `ProcessRegistry::read_output` / `write_stdin` use the raw `BackgroundProcess*` outside
  the registry mutex (`process_registry.cpp:566-611`) -- UAF if `remove()` ever gets a
  caller (it has none today). Guard-comment it or fix before wiring remove to any UI.
- `TabUi* ui` held across the nested modal loop in `on_agent_tool_pending`
  (`agent_event_router.cpp:59,92-111`); tray-quit during an approval dialog can destroy
  the frame under the handler. Narrow (modal disables the frame).
- `active_tab_ui()` fallback derefs `begin()` on an empty map and mis-picks on the "+"
  placeholder selection (`locus_frame.cpp:744-753`). Callers currently pre-guard.
- Partial line at MCP EOF is discarded (`stdio_transport.cpp:395-420`); no hang (pending
  promises are failed). A never-newline server grows `carry` unboundedly.
- `McpClient::start()` can overwrite a `failed` status back to `ready` on a dead
  transport (`mcp_client.cpp:88-113`); self-heals via `list_tools` throwing.
- Windows `~StdioTransport` closes `stdin_w_` without `write_mu_` (`stdio_transport.cpp:43`).
- `AgentTurnRunner` captures `captured_success = true` and never sets it
  (`agent_turn.cpp:346`); harmless today because `observe_plan_tool_result` ignores the
  param -- delete the param or wire it.
- `on_agent_embedding_progress` uses `frame_.file_tree_panel_` unguarded while the sibling
  handler null-checks (`agent_event_router.cpp:295` vs `:318`). Consistency only.

---

## Load-bearing guards (verified present -- do NOT remove in refactors)

1. `FrontendRegistry::broadcast` holds `mutex_` across the ENTIRE fan-out and
   `unregister_frontend` takes the same mutex (`frontend_registry.h:43-53`) -- returning
   from unregister is the only in-flight fence. Do not "optimize" to snapshot-then-call.
2. `teardown_tab_ui` ordering: unregister frontend -> remove proc sink ->
   `terminal_panel_->set_state(nullptr)` -> erase `TabUi` -> `RemovePage`, all BEFORE
   `session_.close_tab` (`locus_frame.cpp:696-718`).
3. The `find_tab_ui(evt.GetId())` null-guard idiom in every router handler -- what makes
   late-delivered wxThreadEvents harmless. Every new handler must keep it.
4. tab_id monotonicity, never reused; 0 reserved for `shared_bridge_` and doubles as the
   not-found sentinel (`locus_session.h:126`, `locus_frame.cpp:254,686-694`).
5. `AgentCore::stop` wakes queue_cv_ AND `dispatcher_->wake()` before joining
   (`agent_core.cpp:388-395`); `~LocusTab` stops the agent before ProcessRegistry teardown.
6. `ToolApprovalDialog` always answers (decision_sent_ + close-reject + post-modal
   fallback) -- every exit path unblocks the dispatcher condvar.
7. `open_workspace` sequencing (`locus_app.cpp:335-386`): detach -> `Destroy()` ->
   `DeletePendingObjects()` -> `session_.reset()` -> spawn. The DeletePendingObjects is
   the only thing containing A4 across workspace switches.
8. `Workspace::~` explicit order (`workspace.cpp:203-223`): `processes_.reset()` ->
   `watcher_pump_->stop()` (final flush while indexer+worker alive) ->
   `embedding_worker_->stop()` -> `watcher_->stop()`. Plus member decl order:
   `embedding_worker_` after `vectors_db_` (worker joined before DB dies).
9. `WatcherPump::stop()` sets `stopping_` under `mutex_` before `notify_all`
   (`watcher_pump.cpp:34-38`) -- the correct condvar discipline A5 wants copied, not the
   reverse.
10. `Indexer::process_mutex_` serialises the pump thread vs `flush_now()` callers.
11. `FileWatcher::stop()` resets the efsw watcher (joining its thread) before the members
    the listener touches die (`file_watcher.cpp:72-83`).
12. `McpClient::stop()` sets `status_ = stopped` BEFORE `transport_->terminate()`
    (`mcp_client.cpp:37-42`) -- suppresses spurious crash callbacks and prevents the A1
    deadlock on the user-initiated stop path.
13. Pending-map drain on every MCP exit path (`mcp_client.cpp:44-55,291-302` + send-side
    erases) -- why no caller ever blocks forever.
14. `stop_all()` swaps `servers_` out under `mu_` and destroys outside it
    (`mcp_manager.cpp:145-150`) -- the pattern `restart()` must adopt (A1).
15. Windows terminate order: `TerminateJobObject` before `CancelIoEx`, no `write_mu_`
    (`stdio_transport.cpp:373-384`) -- the property the POSIX path is missing (B3).
16. `ProcessSinkBroker` emits under `mu_` (`process_sink.h:64-87`) -- `remove_sink` is a
    hard unsubscription barrier. Matching contract: no add/remove inside a callback.
17. `servers_` is `vector<unique_ptr<ServerEntry>>` (`mcp_manager.h:97`) -- `entry_ptr`
    captured in `on_crash` survives vector growth only via the extra indirection.
18. `McpClient` member order: `transport_` declared last-ish so its dtor joins the reader
    before the members the reader touches die (`mcp_client.h:89-104`).
19. `LocusSession::~` nulls `on_activity` before `tabs_.clear()` (`locus_session.cpp:243-251`);
    member order `mcp_` after `tools_` so `~McpManager` runs FIRST and unregisters its
    `McpTool` entries from the still-alive registry before their clients die.
20. `router_` is a plain member destroyed before `~wxEvtHandler` discards pending events --
    safe only because no dispatch occurs during frame teardown. Never add
    `Yield()`/`ProcessPendingEvents()` to frame teardown.
21. `suppress_placeholder_trigger_` around every teardown+close pair -- prevents the "+"
    pseudo-tab auto-select from recreating the tab being closed.
22. `install_tab_ui` registers the frontend only AFTER the `TabUi` is in `tabs_ui_`
    (`locus_frame.cpp:659-662`) -- register immediately broadcasts events.

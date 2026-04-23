# Agent Loop

How `AgentCore` orchestrates a single user turn: threading, phases, callbacks, and the
extension points M4 features (plan mode, checkpoints, auto-verify, parallel tools) will hook
into.

This is a cross-cutting guide. For the owned data structures see
[src/agent/agent_core.h](../src/agent/agent_core.h); for tool execution mechanics see
[tool-protocol.md](tool-protocol.md); for the full system picture see [overview.md](overview.md).

---

## 1. Responsibilities

`AgentCore` is the single turn orchestrator. After S3.A it is a thin composition root — the
heavy lifting lives in four collaborators that are owned via `unique_ptr` and wired up in the
constructor. Everything else (index, tools, LLM, frontends) is external; `AgentCore` binds them
together into a conversation.

| Concern | Owner | Header |
|---|---|---|
| Thread + message queue | `AgentCore::agent_thread_func()` + `queue_cv_` | [agent_core.h](../src/agent/agent_core.h) |
| One LLM round (schema build, streaming, usage accounting) | `AgentLoop::run_step` | [agent_loop.h](../src/agent/agent_loop.h) |
| Tool approval + execute + inject | `ToolDispatcher::dispatch` | [tool_dispatcher.h](../src/agent/tool_dispatcher.h) |
| Token accounting + overflow policy | `ContextBudget` | [context_budget.h](../src/agent/context_budget.h) |
| Activity ring buffer + frontend fan-out of events | `ActivityLog` | [activity_log.h](../src/agent/activity_log.h) |
| Slash-command parse + dispatch + autocomplete | `SlashCommandDispatcher` | [slash_commands.h](../src/agent/slash_commands.h) |
| Compaction | `AgentCore::compact_context`, delegating to `ConversationHistory` | [conversation.h](../src/agent/conversation.h) |
| Session I/O | `SessionManager` | [session_manager.h](../src/agent/session_manager.h) |
| System prompt (+ attached context) | `SystemPromptBuilder` + `AgentCore::compose_system_prompt` | [system_prompt.h](../src/agent/system_prompt.h) |
| Frontend fan-out | `FrontendRegistry` | [frontend_registry.h](../src/frontend_registry.h) |
| Tool-facing workspace surface | `IWorkspaceServices` (implemented by `Workspace`) | [core/workspace_services.h](../src/core/workspace_services.h) |

**Invariant:** only the agent thread writes `history_`. Collaborators never hold conversation
history directly — `AgentLoop::run_step` returns an `AgentStepResult` that `AgentCore` appends,
and `ToolDispatcher::dispatch` receives a `ChatMessage` append callback that runs on the agent
thread. This keeps `history_` lock-free and single-writer.

---

## 2. Threading model

```
┌────────────────────┐    send_message()    ┌─────────────────────┐
│  UI / CLI thread   │─────────────────────▶│  message_queue_     │
│ (also: tool_       │     push + notify    │  (mutex + cv)       │
│  decision, cancel) │                      └──────────┬──────────┘
└────────────────────┘                                 │ pop
                                                       ▼
                                           ┌──────────────────────┐
                                           │   Agent thread       │
                                           │   agent_thread_func  │
                                           │   (single worker)    │
                                           └──────────┬───────────┘
                                                      │
              ┌───────────────────────────────────────┼───────────────────────────────┐
              ▼                                       ▼                               ▼
    IFrontend callbacks                 decision_cv_.wait()              tools_.find() → execute
    (on_token, on_tool_call_pending,    (released when UI thread or      (ran on agent thread)
     on_tool_result, ...)               cancel_turn() notifies)
    fired on agent thread
```

Key rules:

- `send_message()` is **non-blocking** from any thread. It enqueues and returns.
- `send_message_sync()` (CLI) enqueues, then blocks on `sync_cv_` until the turn finishes.
- Every `IFrontend` callback fires on the **agent thread**. wxWidgets frontend marshals to
  the UI thread via `wxQueueEvent` ([src/gui/wx_frontend.h](../src/gui/wx_frontend.h)); the CLI
  is already on the main thread (see `send_message_sync`).
- `tool_decision()` may be called from **any** thread. `AgentCore` forwards it to
  `ToolDispatcher::submit_decision`, which grabs the dispatcher-owned `decision_mutex_` and
  notifies its `decision_cv_`.
- `cancel_turn()` sets `cancel_requested_` and calls `dispatcher_->wake()` so a turn blocked on
  approval unwinds; `stop()` additionally notifies `queue_cv_` so the worker loop exits when
  the queue is empty. `cancel_requested_` is a reference shared with `ToolDispatcher` — the
  dispatcher sees the flag without any extra signalling.

---

## 3. Turn lifecycle

A "turn" is one user message → zero or more LLM calls with tool dispatch in between → final
text-only assistant message. The loop caps at `k_max_rounds = 20` to prevent runaway tool
chains.

### 3.1 Entry points

```
send_message(content)           enqueue, return                  non-blocking
send_message_sync(content)      enqueue, wait for sync_cv_       blocking (CLI)
```

Both paths funnel to `agent_thread_func()` → `process_message()`.

### 3.2 `AgentCore::process_message` phases

```
process_message(content)
 ├─ busy_ = true; cancel_requested_ = false
 ├─ on_turn_start broadcast                              ──▶ all frontends
 ├─ slash_->try_dispatch(content, token_cb, error_cb)    ── if true: on_turn_complete, return
 ├─ history_.add(user message)
 ├─ activity_->emit(user_message)
 ├─ on_context_meter broadcast
 │
 ├─ for round = 1..k_max_rounds:
 │    ├─ if cancel_requested_: emit on_error, break
 │    ├─ budget_->check_overflow(current_token_count())  ── may emit on_compaction_needed, break if full
 │    ├─ step = loop_->run_step(history_)                ◀── AgentLoop
 │    ├─ on_context_meter broadcast
 │    ├─ if step.had_error: break
 │    ├─ history_.add(step.assistant_msg)                ◀── only writer of history_
 │    ├─ if step.tool_calls.empty(): break
 │    └─ for each call in step.tool_calls:
 │         ├─ if cancel_requested_: break
 │         └─ dispatcher_->dispatch(call, append_fn)     ◀── ToolDispatcher
 │
 ├─ if round >= k_max_rounds: on_error broadcast
 ├─ on_turn_complete broadcast
 └─ busy_ = false
```

`append_fn` is a lambda that calls `history_.add(msg)`; `ToolDispatcher` never touches history
directly. Round `1` is always "LLM produces text and/or requests tool calls." Subsequent rounds
only happen when the previous one emitted tool calls — each tool result injects a `tool`
message and the loop asks the LLM to continue.

### 3.3 `AgentLoop::run_step` — one LLM round

```
run_step(history) -> AgentStepResult
 ├─ build_tool_schemas()  ── read from IToolRegistry
 ├─ llm_.stream_completion(history, schemas, callbacks):
 │    ├─ on_token          ──▶ accumulated_text       + on_token broadcast
 │    ├─ on_reasoning_token──▶ accumulated_reasoning  + on_reasoning_token broadcast
 │    ├─ on_tool_calls     ──▶ tool_call_requests = calls
 │    ├─ on_complete       ──▶ trace log
 │    ├─ on_error          ──▶ on_error broadcast, activity_.emit(error), had_error=true
 │    └─ on_usage          ──▶ budget_.set_server_total(total_tokens)
 │
 ├─ if had_error: return { had_error=true }
 ├─ activity_.emit(llm_response) with tokens_in, tokens_delta (via ContextBudget)
 └─ return AgentStepResult {
        assistant_msg = { role=assistant, content=accumulated_text, tool_calls=... },
        tool_calls    = parsed ToolCall list,
        had_error     = false
    }
```

The loop owns neither tool execution nor history mutation — it is a pure "build a request,
stream a response, hand back the pieces" component. Tool-call parsing (id/name/args) happens
inside the loop via `ToolRegistry::parse_tool_call`; the dispatcher receives fully parsed
`ToolCall` values.

### 3.4 `ToolDispatcher::dispatch` — approval gate + execute + inject

```
dispatch(call, append_result)
 ├─ tool = tools_.find(call.tool_name)
 ├─ if !tool: append tool-error message + on_error + activity_.emit(error); return
 │
 ├─ activity_.emit(tool_call, summary=name, detail=args)
 ├─ policy = services_.effective_approval_policy(call.tool_name, tool->approval_policy())
 │          (IWorkspaceServices consults workspace config for per-tool overrides)
 │
 ├─ if policy == deny:
 │     append "denied by policy" tool message, return
 │
 ├─ if policy == manual:
 │     preview = tool->preview(call)
 │     on_tool_call_pending broadcast                ──▶ frontend shows approval UI
 │     decision_cv_.wait until pending_decision_ || cancel_flag_.load()
 │     if cancelled: append "cancelled", return
 │     if decision == reject: append "rejected", return
 │     if decision == modify && !mod_args.empty(): effective_call.args = mod_args
 │
 ├─ result = tool->execute(effective_call, services_)         ◀── runs on agent thread
 ├─ activity_.emit(tool_result or error)
 ├─ append_result(ChatMessage{tool, tool_call_id, result.content})
 └─ on_tool_result broadcast (display text, not raw content)
```

`auto_approve` tools skip the `on_tool_call_pending` step entirely — the frontend never sees
an approval event, only `on_tool_result`. The dispatcher holds its own `decision_mutex_` /
`decision_cv_`; `AgentCore::tool_decision` forwards the user choice via
`submit_decision(decision, modified_args)`, and `cancel_turn()` calls `wake()` to release
`decision_cv_` so the dispatcher can observe the shared `cancel_flag_` and unwind.

---

## 4. Sequence diagrams

### 4.1 Simple text-only turn

```mermaid
sequenceDiagram
    autonumber
    participant UI as UI thread
    participant AG as Agent thread
    participant LLM as ILLMClient

    UI->>AG: send_message("hi")
    AG->>UI: on_turn_start
    AG->>AG: history.add(user)
    AG->>AG: emit_activity(user_message)
    AG->>LLM: stream_completion(history, schemas)
    loop tokens
        LLM-->>AG: on_token("Hel"), on_token("lo")
        AG->>UI: on_token, on_token
    end
    LLM-->>AG: on_usage, on_complete (no tool_calls)
    AG->>AG: history.add(assistant)
    AG->>AG: emit_activity(llm_response)
    AG->>UI: on_context_meter
    AG->>UI: on_turn_complete
```

### 4.2 Turn with an approval-gated tool call

```mermaid
sequenceDiagram
    autonumber
    participant UI as UI thread
    participant AG as Agent thread
    participant LLM as ILLMClient
    participant T as ITool

    UI->>AG: send_message("read foo.cpp")
    AG->>UI: on_turn_start

    Note over AG,LLM: Round 1
    AG->>LLM: stream_completion
    LLM-->>AG: on_tool_calls([read_file(foo.cpp)])
    AG->>AG: history.add(assistant with tool_calls)

    AG->>UI: on_tool_call_pending(call, preview)
    Note over AG: decision_cv_.wait()
    UI->>AG: tool_decision(approve)
    Note over AG: wait released

    AG->>T: execute(call, services_)
    T-->>AG: ToolResult{success, content, display}
    AG->>AG: history.add(tool, call_id, content)
    AG->>UI: on_tool_result(call_id, display)

    Note over AG,LLM: Round 2
    AG->>LLM: stream_completion (history now has tool result)
    loop tokens
        LLM-->>AG: on_token("The file ...")
        AG->>UI: on_token
    end
    LLM-->>AG: on_complete (no tool_calls)
    AG->>AG: history.add(assistant)
    AG->>UI: on_turn_complete
```

### 4.3 Cancellation while waiting for approval

```mermaid
sequenceDiagram
    autonumber
    participant UI as UI thread
    participant AG as Agent thread

    AG->>UI: on_tool_call_pending(call)
    Note over AG: decision_cv_.wait()
    UI->>AG: cancel_turn()
    Note over AG: cancel_requested_ = true;<br/>decision_cv_.notify_one()
    Note over AG: wait predicate sees<br/>cancel_requested_ and returns
    AG->>AG: inject "[cancelled]" tool message
    AG->>UI: on_tool_result(call_id, "[cancelled]")
    AG->>UI: on_error("Turn cancelled.")
    AG->>UI: on_turn_complete
```

### 4.4 Multi-round tool chain (3 rounds, 4 tools)

```mermaid
sequenceDiagram
    autonumber
    participant AG as Agent thread
    participant LLM

    AG->>LLM: round 1
    LLM-->>AG: tool_calls=[search_symbols, read_file]
    AG->>AG: dispatch (sequential, approval per call)
    AG->>LLM: round 2
    LLM-->>AG: tool_calls=[read_file]
    AG->>AG: dispatch
    AG->>LLM: round 3
    LLM-->>AG: text only
    Note over AG: loop exits (has_tool_calls=false)
```

Each round is one `AgentLoop::run_step()` call. Round count is bounded by `k_max_rounds = 20`;
hitting the cap emits `on_error("Agent reached the maximum number of tool call rounds.")` and
ends the turn.

---

## 5. M4 extension points

The phases above are the surface area M4 features attach to. The M3 refactor (S3.A / S3.C /
S3.J) gave each of these a clean seam before they pile on. Hook points, mapped to the phase
they fire around:

| Hook | Phase | Used by | Notes |
|---|---|---|---|
| `before_turn` | start of `process_message` | plan mode (S4.D), memory bank (S4.R) | inject mode-specific system-prompt variant; recall relevant memories |
| `after_user_message` | after `history.add(user)`, before round 1 | plan mode, multi-model router (S4.Q) | choose model / mode for this turn |
| `before_llm_step` | top of `AgentLoop::run_step` | KV-cache optimizer (S4.F), router (S4.Q) | pick model, slice history, inject system-prompt deltas |
| `after_llm_step` | after `history.add(assistant)` | streaming tool results (S4.O), telemetry (S4.S) | |
| `before_tool_dispatch` | top of `ToolDispatcher::dispatch`, after policy resolve | plan mode (blocks tools), LSP (S4.E), parallel dispatch (S4.H) | plan mode rejects any tool except `propose_plan`; LSP augments `write_file` pre-hook |
| `after_tool_success` | after `tool->execute()` returns success, before `history.add(tool)` | **checkpoint/undo (S4.B)**, **auto-verify (S4.C)** | checkpoint copies prior file state before next mutation; verify runs `verify.cmd` and injects `<verify_failed>` on non-zero exit |
| `after_tool_failure` | after `tool->execute()` returns failure | telemetry, retry policy | |
| `after_turn` | just before `on_turn_complete` broadcast | checkpoint GC, memory bank write-back, telemetry | |

S3.A landed these as method seams on `AgentLoop` / `ToolDispatcher` rather than a formal
observer registry — a subscription API is overkill while there is exactly one `AgentCore` per
workspace. An explicit observer interface can come later if a second consumer per hook appears.

### 5.1 Plan mode (S4.D)

Plan mode is a `process_message`-level state, not a new phase:

- A third agent mode enum (`chat` | `plan` | `execute`) lives on `AgentCore`.
- `plan` swaps `system_prompt_` to a variant that forbids tool calls except `propose_plan`.
- `before_tool_dispatch` rejects every other tool name with a `plan_mode_violation` tool
  message so the LLM can self-correct in the same round.
- Approving the plan flips the mode to `execute`; the plan text is pinned into
  `AttachedContext`-like slot so it survives compaction.

### 5.2 Checkpoint/undo (S4.B)

- `after_tool_success` snapshots prior file state for any mutating tool result, keyed by
  `(session_id, turn_id)`.
- A `turn_id` counter — currently implicit in round iteration — becomes an explicit member on
  `AgentCore` and is included in `ActivityEvent`.
- `SessionStore` (to be promoted from `SessionManager`) gains `undo_turn(session_id,
  turn_id)` which restores files and emits an activity event.

### 5.3 Auto-verify (S4.C)

- `after_tool_success` checks `.locus/config.json` `verify.trigger` against the tool's mutation
  kind (`on_edit` vs `on_turn_end` vs `manual`).
- On non-zero exit, injects an extra tool-like message tagged `<verify_failed>` carrying
  stdout/stderr tail. The LLM sees it in round `N+1` and can react within the same turn.
- Zero exit logs only — token discipline forbids injecting quiet successes.

### 5.4 Parallel tool dispatch (S4.H)

- Today's inner loop is `for each call in step.tool_calls: dispatcher_->dispatch(call, ...)`
  in `AgentCore::process_message`.
- Parallel dispatch replaces this with a fan-out: each independent call runs on a worker,
  results are collected in the original order and fed back through the same `AppendFn` so
  `AgentCore` remains the sole writer of `history_`.
- Approval UI already supports multiple pending calls per round via `call_id` — the
  constraint is on tool-level declarations of whether a tool is parallel-safe, not on
  `AgentCore`'s plumbing.

---

## 6. Context budget and compaction

`AgentCore::current_token_count()` delegates to `ContextBudget::current(fallback_estimate)`,
which returns the **server-reported** total when available (`server_total_`), falling back to
`history_.estimate_tokens()` when the backend (e.g. older LM Studio) omits the `usage` field.

```
ContextBudget::check_overflow(used):
  ratio = used / limit
  if ratio >= 1.0:            on_compaction_needed, return true  (caller breaks the round loop)
  if ratio >= 0.80:           on_compaction_needed, return false (turn continues)
```

Compaction itself is user-driven, never automatic:

| Strategy | Implementation | Fires |
|---|---|---|
| `drop_tool_results` (B) | `ConversationHistory::drop_tool_results()` | any time |
| `drop_oldest` (C) | `ConversationHistory::drop_oldest_turns(n)` | any time |

Strategy A ("LLM summarize") and D ("save and restart") live in [overview.md](overview.md) §
Context Management and are not yet implemented in `AgentCore`.

The seed system message at `history_.messages()[0]` is **never compacted**:
`refresh_system_prompt()` rewrites it in place when `AttachedContext` changes. Compaction
routines start from index 1.

---

## 7. Activity events

Every externally-visible moment in a turn emits an `ActivityEvent` to the ring buffer owned by
`ActivityLog` and broadcasts `on_activity` to all frontends. Kinds map to phases:

| Kind | Emitted from | Notes |
|---|---|---|
| `system_prompt` | `AgentCore` ctor | once at startup, records seeded tokens |
| `user_message` | `AgentCore::process_message` | after `history.add(user)` |
| `llm_response` | end of `AgentLoop::run_step` | carries `tokens_in`, `tokens_delta`, plus `[thinking]` if any reasoning was streamed |
| `tool_call` | start of `ToolDispatcher::dispatch` | before approval wait |
| `tool_result` | after successful `tool->execute` (via `ToolDispatcher`) | |
| `error` | LLM error, unknown tool, tool failure | |
| `index_event` | external subsystems via `AgentCore::emit_index_event` → `ActivityLog::emit_index_event` | indexer, embedding worker |

Late-joining frontends (web clients, mid-session IDE attach) call
`AgentCore::get_activity(since_id=N)`, which delegates to `ActivityLog::get_since(N)`, to catch
up without replaying conversation history. Ring buffer cap: `k_activity_buffer_max = 1000`.

---

## 8. Invariants and gotchas

**Do not break these:**

1. **Single-writer history.** Only the agent thread writes `history_`. Collaborators never
   hold it directly — `AgentLoop` returns an `AgentStepResult` that `AgentCore` appends, and
   `ToolDispatcher` receives an `AppendFn` callback. Any M4 hook that wants to append
   (checkpoint's turn marker, verify's injection) must do so via that same callback running
   on the agent thread.
2. **Approval wait is cancellable.** `ToolDispatcher::dispatch` waits on `decision_cv_` with a
   predicate that includes `cancel_flag_.load()`. `AgentCore::cancel_turn` sets the shared
   `cancel_requested_` atomic and calls `dispatcher_->wake()` to notify the condvar. Any new
   wait path must honour the same pattern, or `cancel_turn()` hangs.
3. **`sync_cv_` must always fire.** `send_message_sync` blocks on it — every exit path from
   `process_message` (normal completion, error, round cap, cancellation) relies on the
   `sync_turn_done_ = true; sync_cv_.notify_all()` at the bottom of `agent_thread_func`.
4. **Frontend callbacks never throw across the boundary.** `FrontendRegistry::broadcast`
   isolates exceptions per frontend — individual frontend bugs cannot take down the agent
   thread. Preserve this when adding fan-out points.
5. **`tool_decision()` is thread-agnostic.** It's called from the UI thread in all current
   frontends, but may come from a websocket thread once `CrowServer` ships (M5). `AgentCore`
   forwards it to `ToolDispatcher::submit_decision` which takes the dispatcher's own mutex —
   do not add thread-affinity assumptions on either side.
6. **Cancellation is cooperative.** `cancel_requested_` is checked between rounds and between
   tool calls within a round; it does **not** interrupt an in-flight `llm_.stream_completion`
   or `tool->execute`. Those must complete before the cancel propagates.
7. **Tool-facing surface is `IWorkspaceServices`, not `Workspace`.** Tools receive
   `services_` and must not downcast to the concrete `Workspace`. This keeps tool tests
   hermetic (via `FakeWorkspaceServices`) and allows M5 to add remote/virtual workspaces.

---

## 9. Where to look next

- [src/agent/agent_core.cpp](../src/agent/agent_core.cpp) — composition root, thread + queue, `process_message`
- [src/agent/agent_loop.cpp](../src/agent/agent_loop.cpp) — single LLM round: schema, streaming, usage
- [src/agent/tool_dispatcher.cpp](../src/agent/tool_dispatcher.cpp) — approval gate + execute + inject
- [src/agent/activity_log.cpp](../src/agent/activity_log.cpp) — ring buffer + frontend fan-out
- [src/agent/context_budget.cpp](../src/agent/context_budget.cpp) — token accounting, overflow policy
- [src/agent/slash_commands.cpp](../src/agent/slash_commands.cpp) — parser + dispatcher + autocomplete
- [src/agent/conversation.h](../src/agent/conversation.h) — `ConversationHistory`, `ChatMessage`, token estimation
- [src/core/workspace_services.h](../src/core/workspace_services.h) — `IWorkspaceServices` tool-facing surface
- [src/frontend.h](../src/frontend.h) — `IFrontend`, `ILocusCore`, `ToolDecision`, `CompactionStrategy`
- [tool-protocol.md](tool-protocol.md) — `ITool`, approval policies, adding new tools
- [overview.md](overview.md) — system-wide component map and context strategy
- [decisions/](decisions/) — ADR trail for load-bearing architectural decisions
- [roadmap/M3/S3.A-agent-core-split.md](../roadmap/M3/S3.A-agent-core-split.md) — the extraction of `AgentLoop`, `ToolDispatcher`, `ActivityLog`, `ContextBudget` (done)
- [roadmap/M4/S4.B-checkpoint-undo.md](../roadmap/M4/S4.B-checkpoint-undo.md), [S4.C-auto-verify.md](../roadmap/M4/S4.C-auto-verify.md), [S4.D-plan-mode.md](../roadmap/M4/S4.D-plan-mode.md) — features that will use the hook points above

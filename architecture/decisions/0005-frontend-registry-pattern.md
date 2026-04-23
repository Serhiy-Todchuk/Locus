# 0005. Multi-frontend fan-out via `FrontendRegistry`

- **Status**: Accepted
- **Date**: 2026-04-23

## Context

`AgentCore` emits a continuous stream of events during a turn: `on_token`,
`on_tool_call_pending`, `on_tool_result`, `on_context_meter`, `on_activity`,
`on_compaction_needed`, `on_turn_start`, `on_turn_complete`, and so on. Each event needs a
consumer that renders it (UI), prompts the user (approval), or forwards it over the wire
(remote client).

The original S0.7/S1.1 design had `AgentCore` hold a single `IFrontend*` member and call it
directly. That worked while only the CLI existed. The roadmap then needed multiple
simultaneous consumers:

- **CLI + GUI coexisting during development.** Two frontends attached to the same agent so
  the CLI could log what the GUI was showing.
- **External clients via `CrowServer` (M5).** The Crow server is itself an `IFrontend` that
  fans out to N WebSocket clients. The agent should not know or care how many remote
  browsers, VS Code shims, or mobile apps are listening.
- **Late-joining frontends.** Mid-session attach (a browser opens while the agent is already
  processing a turn) needs the history of activity events, not the live stream alone.

With a single-pointer design, each of these meant threading a second pointer through
`AgentCore` or inventing ad-hoc multiplexers at the call sites. Worse, any frontend that
threw from a callback would crash the agent thread.

## Decision

Introduce `FrontendRegistry` — a thread-safe, exception-isolated list of `IFrontend*`
owned by `AgentCore` — and route every outbound event through it.

Properties:

- **N registered frontends, symmetric delivery.** `register_frontend` / `unregister_frontend`
  are the only ways to attach; every registered frontend receives every event through
  `broadcast(fn)`.
- **Mutex-protected.** Registration and fan-out share one `std::mutex`. Holding the mutex
  across `fn(*fe)` is acceptable because callbacks are short (marshal to UI queue, write to
  a socket); long work is always deferred to the frontend's own thread.
- **Exception-isolated.** `broadcast` wraps each call in `try/catch` and logs via `spdlog`.
  A buggy frontend cannot take the agent thread down.
- **Catch-up is separate.** Streaming events go through `FrontendRegistry`; the
  `ActivityLog` ring buffer owns the history. A late-joining frontend calls
  `AgentCore::get_activity(since_id=N)` once on attach and then consumes live events from
  the registry. No replay semantics baked into the registry itself.
- **Header-only.** Small enough to live in `src/frontend_registry.h`. No .cpp, no
  translation-unit cost.

## Consequences

**Wins**
- `AgentCore` and its collaborators (`AgentLoop`, `ToolDispatcher`, `ActivityLog`,
  `ContextBudget`) hold one `FrontendRegistry&` and do not know how many frontends exist.
  Adding or removing frontends does not touch core code.
- The Crow server becomes "just another frontend" that happens to fan out to WebSockets.
  The C++ direct path (wxWidgets, CLI) and the network path use the exact same
  `IFrontend` contract and the same registry.
- Frontend bugs stay local. A `wxQueueEvent` failure, a broken socket, a UTF-8 decode
  exception — all caught at the registry boundary, logged, and skipped.
- Testing is straightforward: a fake frontend that records events is a drop-in consumer.
  No coupling to a specific singleton or thread model.

**Costs**
- The mutex must be held across every fan-out call. In practice callbacks are dispatcher-
  cheap (post to a queue, append to a buffer), so contention is negligible — but long-running
  work in a callback would serialise all frontends. Enforced by convention, not the type
  system.
- Delivery order is registration order, not semantic priority. Today this does not matter
  (events are advisory; no frontend "wins"), but a future need for ordering would require a
  separate mechanism.
- `broadcast` takes a `std::function`, so each call allocates a small closure on the stack
  and pays one indirect call per frontend. Trivial at the rate events fire.

## Alternatives considered

- **Single `IFrontend*`, multiplexer lives outside.** Moves the fan-out problem to every
  caller (CLI bootstrap, GUI bootstrap, Crow startup) instead of solving it once. Every new
  lifecycle combination re-invents the same loop. Rejected.
- **Observer pattern with typed signals per event.** Boost.Signals2-style. More ceremony
  (one signal per `IFrontend` method), no real win — `broadcast([](IFrontend& fe){ ... })`
  is already as concise as a slot connect. Also pulls in a dependency we don't need.
- **Lock-free copy-on-write list for frontends.** The registration rate is approximately
  once-per-frontend-lifetime. Optimising for lock-free reads here is premature — the mutex
  is never the bottleneck.
- **Let exceptions propagate.** Rejected as a correctness hazard. A frontend throwing would
  abort the entire turn, corrupt `busy_`/`sync_turn_done_` state, and hang
  `send_message_sync`. Frontend isolation is the whole point.
- **Deliver events on a dedicated dispatcher thread.** Adds a thread, a queue, and ordering
  questions (is a `on_tool_result` allowed to arrive before the `on_tool_call_pending` for
  a *later* call?) for no current benefit. The agent thread is already the natural serialiser.

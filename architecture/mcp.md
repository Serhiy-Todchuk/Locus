# MCP -- Model Context Protocol Integration

S4.G makes Locus an MCP client. Workspace-scoped MCP servers are spawned at session startup, their advertised tools join the agent's tool catalog under the `mcp:<server>:<tool>` namespace, and tool invocations round-trip through stdio JSON-RPC.

Phase 1 landed the engine + a bundled mock server for tests. Phase 2 adds a Settings panel, a per-server trust toggle, a 1 MB result cap, explicit child-exit detection, and `IToolRegistry::unregister_tool` (so hot-restart can replace stale tools cleanly).

This document is the user-facing reference for `mcp.json`. Engine internals are described in code -- see [src/mcp/](../src/mcp/).

---

## File locations

Two files are merged at session startup. Workspace entries override global by server name; everything else is additive.

| Location | Path | Purpose |
|---|---|---|
| Workspace | `<workspace-root>/.locus/mcp.json` | Servers specific to this project. Commit it to share with collaborators. |
| Global    | `%APPDATA%/Locus/mcp.json` (Windows)<br>`$XDG_CONFIG_HOME/Locus/mcp.json` or `~/.config/Locus/mcp.json` (other) | Servers you want across every workspace -- e.g. `time`, `fetch`, your personal LSP. |

Missing files are not errors. JSON parse failures log a warning and skip the offending file -- a malformed global file does not stop the workspace one from loading.

---

## Schema

```json
{
  "mcpServers": {
    "<server_name>": {
      "command": "executable-or-script",
      "args":    ["arg1", "arg2"],
      "env":     { "KEY": "value" },
      "cwd":     "optional-working-directory",
      "enabled": true,
      "init_timeout_ms": 10000
    }
  }
}
```

| Key | Type | Required | Default | Notes |
|---|---|---|---|---|
| `command` | string | **yes** | -- | Executable name (PATH-searched) or absolute path. Windows: `.exe` extension is required for direct invocation. Relative paths resolve against the parent process's cwd, not against `cwd` below. |
| `args` | array of strings | no | `[]` | Each element is passed verbatim. Spaces in elements are quoted automatically. |
| `env` | object | no | `{}` | Merged on top of the parent environment. Existing keys are overwritten (case-insensitive on Windows). |
| `cwd` | string | no | workspace root | Child's working directory. Useful for servers that read config relative to their own folder. |
| `enabled` | bool | no | `true` | When false the server is parsed but not spawned. Visible in the Settings UI as a disabled entry. |
| `init_timeout_ms` | int | no | `10000` | How long to wait for the server's `initialize` reply. First-time `npx -y @scope/server` runs can need the full 10 s; servers that miss the window are marked `failed`. |

The schema mirrors the de-facto format used by Claude Desktop, Cursor, Cline, and the official `@modelcontextprotocol/inspector` -- a config file written for one is mostly portable to Locus and back.

---

## Worked examples

### Filesystem server (npx)

```json
{
  "mcpServers": {
    "fs": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "."],
      "enabled": true
    }
  }
}
```

After session restart the agent sees `mcp:fs:read_file`, `mcp:fs:list_directory`, etc. The first launch pulls the package from npm (slow); subsequent runs are fast.

### Postgres MCP server with credentials

```json
{
  "mcpServers": {
    "pg": {
      "command": "uvx",
      "args": ["mcp-server-postgres", "--read-only"],
      "env": {
        "DATABASE_URL": "postgresql://reader:secret@localhost:5432/app"
      },
      "init_timeout_ms": 30000
    }
  }
}
```

`env.DATABASE_URL` is set only for the child; it doesn't leak into Locus's process or any other server.

### Disabling a server temporarily

```json
{
  "mcpServers": {
    "fs":   { "command": "npx", "args": ["-y", "@modelcontextprotocol/server-filesystem", "."], "enabled": false },
    "time": { "command": "uvx", "args": ["mcp-server-time"] }
  }
}
```

`fs` is parsed and shown in Settings but not started, so its tools don't enter the catalog. `time` runs normally.

---

## Tool namespacing

Every MCP tool reaches the LLM under `mcp:<server>:<tool>`:

| Server `inputSchema.name` | Visible to the LLM as |
|---|---|
| `read_file`     | `mcp:fs:read_file` |
| `query`         | `mcp:pg:query` |

The namespace is enforced by `McpTool::name()` -- there's no way to register an MCP tool under a bare name. Built-in tools and MCP tools can never collide, and the source of any tool invocation is visually obvious in the chat + activity log.

---

## Approval and trust

MCP tools are untrusted by default: `McpTool::approval_policy()` returns `ask`, so every invocation goes through the approval gate.

Three ways to grant trust:

1. **Per-tool** -- Settings -> Tool Approvals tab, set `mcp:fs:read_file` to `Auto-Approve`.
2. **Per-server** (S4.G phase 2) -- Settings -> MCP Servers tab, select the server, tick **Trust selected server**. This writes `mcp:<server>:*` -> `auto_approve` into `tool_approval_policies`; `ToolDispatcher` matches it as a prefix.
3. **Hand-edit `.locus/config.json`** -- any key ending in `:*` matches as a prefix. Exact matches beat prefix matches.

A bare `"*"` wildcard is intentionally not supported. Blanket auto-approve for everything would defeat the entire approval gate.

---

## Result handling

`tools/call` results flow through `McpTool::execute` and land in the chat as:

- **Success:** `result.content[]` is flattened (text parts inlined, resource parts surfaced with a leading `[resource: uri]` tag, image/blob parts replaced with `[<type> content omitted]`). The result is reported as `success=true`.
- **Server-reported error** (`isError: true`): the same flattening runs but the result is reported as `success=false`, prefixed with `Error from MCP server:`.
- **Transport error** (server crashed mid-call, timeout, etc.): a synthesised `Error calling MCP tool 'mcp:...': <reason>` lands as `success=false`. The server is marked `crashed`.

### 1 MB result cap (S4.G phase 2)

Each per-call body is hard-capped at **1 MB**, matching `CheckpointStore`'s per-file cap. Oversized results are truncated with a `[truncated N bytes -- MCP result exceeded the 1 MB cap]` suffix and logged at warn level. The cap protects `ConversationHistory` from a chatty or buggy server flooding the context window before the soft-compaction threshold fires.

---

## Lifecycle and status

`McpManager` runs each server through this state machine:

```
not_started -> initializing -> ready -> stopped (we asked)
                            \-> failed (initialize didn't complete in time)
                            \-> crashed (pipe closed after ready)
```

- **Crash detection** (phase 2): when the child process exits, the reader thread waits up to 2 s for `GetExitCodeProcess` to return a real code (`STILL_ACTIVE` is treated as "didn't manage to wait"), then surfaces it via `ServerStatus::has_exit_code` / `ServerStatus::exit_code`. The Settings MCP tab shows the code next to the status label when present.
- **Hot restart** (phase 2): the Settings panel's **Restart** button calls `McpManager::restart(name)`, which `stop()`s the dead client, unregisters its tools (via `IToolRegistry::unregister_tool`), spawns a fresh `McpClient`, and re-runs `initialize` + `tools/list`. The new advertised tool set replaces the old set in the registry.
- **Tool availability**: `McpTool::available()` returns false unless the client is `ready`. The agent's per-turn tool manifest excludes unavailable tools, so a crashed server stops being offered to the LLM without us having to unregister the entries at crash time.

---

## Testing

Unit tests (`tests/test_mcp.cpp`, `[s4.g][mcp]`): JSON-RPC framing, config merge, stdio handshake, tools/list + tools/call round trip, `isError`, crash lifecycle, manager registration, graceful no-op on bad command, hot-restart unregister cycle, 1 MB result cap.

Integration test (`tests/integration/test_int_mcp.cpp`, `[integration][llm][mcp]`): live LLM picks the namespaced `mcp:mock:echo` tool, the round-trip is asserted via the captured tool call + result. Bundled mock server: [tests/mock_mcp_server/main.cpp](../tests/mock_mcp_server/main.cpp).

Manual end-to-end check (phase 2 exit criterion): point `mcp.json` at `@modelcontextprotocol/server-filesystem` via `npx` and verify a real shipping MCP server registers + responds. The mock alone is not a sufficient signal that we speak MCP correctly -- it only exercises the protocol surface we designed for ourselves.

---

## What this is *not*

- **Not a server.** Locus does not host MCP servers, only spawns + speaks to them.
- **Not a network transport.** Streamable-HTTP MCP transport is parked: every shipping server has a stdio mode.
- **Not cross-platform yet.** `StdioTransport` is Win32-only today. POSIX support is tracked alongside the broader Locus cross-platform effort.

# Tool Protocol

The tool system is the bridge between the LLM's intentions and real actions on the machine.
Tools must be pluggable from day one — new tools should slot in without touching the agent core.

---

## Design Goals

- **Pluggable**: registering a new tool requires only implementing one interface
- **Self-describing**: every tool carries its own schema, fed to the LLM in the system prompt
- **Approval-aware**: tools declare whether they require user approval before execution
- **Display-aware**: tool results have separate LLM content and user-facing display text
- **Stateless**: tools do not hold conversation state; that belongs to the Agent Core

---

## C++ Interface

```cpp
// A single parameter in a tool's input schema
struct ToolParam {
    std::string name;
    std::string type;         // "string" | "integer" | "boolean" | "array" | "object"
    std::string description;  // shown to LLM in schema
    bool        required = true;
};

// What comes back from a tool execution
struct ToolResult {
    bool        success;
    std::string content;      // injected into LLM context (compact, token-efficient)
    std::string display;      // shown to user in UI (may be richer / formatted differently)
    // If success == false, content contains the error description
};

// The LLM's request to invoke a tool (parsed from model output)
struct ToolCall {
    std::string      id;         // from LLM response (e.g. "call_abc123")
    std::string      tool_name;
    nlohmann::json   args;       // validated against the tool's parameter schema
};

// ----- The interface every tool implements -----

class ITool {
public:
    virtual ~ITool() = default;

    // Identity — used by LLM and by the approval UI
    virtual std::string              name()        const = 0;
    virtual std::string              description() const = 0;  // fed verbatim to LLM
    virtual std::vector<ToolParam>   params()      const = 0;

    // Execution — called only after user approval (if required)
    virtual ToolResult execute(const ToolCall& call,
                               IWorkspaceServices& ws) = 0;

    // Approval policy
    // "always"  — pause for user before every execution (default)
    // "auto"    — execute without pausing (read-only tools may use this)
    // "never"   — tool is disabled; attempting to call it returns an error
    virtual std::string approval_policy() const { return "always"; }

    // Optional: called before approval dialog to generate a human-readable
    // summary of what this specific call will do (shown in the approval panel)
    virtual std::string preview(const ToolCall& call) const { return ""; }
};
```

---

## Tool Registry

The registry is the agent core's only dependency on tools.
It knows nothing about individual tool implementations.

```cpp
class IToolRegistry {
public:
    virtual ~IToolRegistry() = default;

    virtual void   register_tool(std::unique_ptr<ITool> tool) = 0;
    virtual ITool* find(const std::string& name) const = 0;
    virtual std::vector<ITool*> all() const = 0;

    // Builds the JSON schema block injected into the LLM system prompt
    // Format: OpenAI function-calling schema (compatible with LM Studio)
    virtual nlohmann::json build_schema_json() const = 0;
};
```

---

## Built-in Tools (v1)

| Tool name | Approval | Description |
|---|---|---|
| `read_file` | auto | Read file contents (paginated) |
| `write_file` | always | Write/overwrite a file |
| `create_file` | always | Create a new file at a path |
| `delete_file` | always | Delete a file (extra confirmation in preview) |
| `list_directory` | auto | List directory tree with index metadata |
| `search_text` | auto | FTS5 full-text search, returns ranked snippets |
| `search_symbols` | auto | Find code symbols by name/kind |
| `get_file_outline` | auto | File structure (headings, symbols) without full content |
| `run_command` | always | Execute a terminal command in workspace dir |
| `web_search` | always | Search the web, return titles + URLs + snippets |
| `web_fetch` | always | Fetch URL, extract text, index in web_fts, return outline |
| `web_read` | auto | Read a section of a fetched web page by heading or line range |

`auto` tools (read-only, non-destructive) execute without pausing for approval.
`always` tools pause and show the approval dialog before running.

---

## Approval Flow

```
LLM output contains tool call
         │
         ▼
  Parse & validate args against schema
         │
         ├─── validation failed → inject error, continue LLM
         │
         ▼
  Check approval_policy()
         │
         ├─── "auto"   → execute immediately
         │
         └─── "always" → show approval dialog
                │   tool name + args + preview() text
                │
                ├─── Approve   → execute → inject result → continue LLM
                ├─── Modify    → user edits args → execute → inject result
                └─── Reject    → inject "user rejected tool call" → continue LLM
```

---

## What a Tool Implementation Looks Like

```cpp
// Example: read_file tool
class ReadFileTool : public ITool {
public:
    std::string name()        const override { return "read_file"; }
    std::string description() const override {
        return "Read the contents of a file. Use offset and length to paginate large files.";
    }
    std::vector<ToolParam> params() const override {
        return {
            { "path",   "string",  "Relative path from workspace root", true  },
            { "offset", "integer", "Line number to start reading from",  false },
            { "length", "integer", "Number of lines to read",            false },
        };
    }
    std::string approval_policy() const override { return "auto"; }

    ToolResult execute(const ToolCall& call, IWorkspaceServices& ws) override {
        // ... read the file, return paginated content
    }
};
```

---

## Adding a New Tool

1. Create a class that inherits `ITool`
2. Implement the 4 required methods: `name`, `description`, `params`, `execute`
3. Register it: `registry.register_tool(std::make_unique<MyTool>())`
4. Done — the agent core picks it up automatically in the next session

No changes to the agent core, no changes to the approval flow, no changes to the prompt builder.

---

## Platform-Specific Tools

Some tools have platform-specific implementations (e.g. `run_command` uses `CreateProcess`
on Windows, `fork`/`exec` on Linux). The `ITool` interface is the same across platforms;
only the implementation class differs. The registry is populated at startup with the
correct implementation for the current platform.

---

## Future: External / Plugin Tools

The same `ITool` interface can be implemented by:
- Tools loaded from a DLL/shared library
- Tools that call out to a subprocess (e.g. a Python script)
- Tools provided by a workspace-local plugin (`LOCUS_TOOLS.md` describing a tool script)

No protocol changes required — if it implements `ITool`, it works.

#pragma once

#include "tool.h"

namespace locus {

class RunCommandTool : public ITool {
public:
    std::string name()        const override { return "run_command"; }
    std::string description() const override {
        return "Execute a shell command synchronously in the workspace directory and "
               "return stdout/stderr + exit code. Use this for commands you expect to "
               "terminate -- builds, tests, scripts, queries. The harness applies a "
               "30-minute wall-clock; the user can hit Stop at any time. "
               "For dev servers, watchers, and anything that streams output without "
               "terminating, use `run_command_bg` instead.\n\n"
               "By default the returned output is smart-truncated to the first 50 + "
               "last 50 lines with the middle elided (configurable per workspace via "
               "`agent.run_command_truncate_lines`). The full raw output is always "
               "written to `.locus/locus.log` at trace level (-verbose). Override "
               "with `output_filter_mode`: `head` / `tail` (kept lines from the "
               "start / end), `head_tail` (default; per-side line count), `regex` / "
               "`substring` (grep-style hits with optional context). For a tight "
               "build-error pass: "
               "`output_filter_mode=\"regex\"`, "
               "`output_filter_pattern=\"error|warning|undefined reference\"`, "
               "`output_filter_lines=20`, `output_filter_context=2`.";
    }
    std::string short_description() const override {
        return "Run a shell command synchronously and return its output.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"command", "string", "The command to execute", true},
            {"output_filter_mode",    "string",
             "How to trim the returned stdout/stderr. One of: `head_tail` "
             "(default; first + last N lines), `head`, `tail`, `regex`, "
             "`substring`. Leave unset for the default smart-truncate.",
             false},
            {"output_filter_pattern", "string",
             "Required for `regex` / `substring` mode. ECMAScript regex or "
             "literal needle to keep matching lines from the output.",
             false},
            {"output_filter_lines",   "integer",
             "Per-side line count for head_tail; total kept lines for "
             "head/tail; max matches for regex/substring. 0 = workspace "
             "default (see `agent.run_command_truncate_lines`).",
             false},
            {"output_filter_context", "integer",
             "Lines of -B/-A context per match for regex/substring "
             "(0..10, default 0).",
             false},
        };
    }
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

// -- S4.I -- background processes -------------------------------------------

class RunCommandBgTool : public ITool {
public:
    std::string name()        const override { return "run_command_bg"; }
    std::string description() const override {
        return "Start a long-running shell command in the background and return its "
               "process_id. The command keeps running across agent turns. Use for dev "
               "servers, file watchers, long builds, or anything you intend to tail. "
               "Read its output with `read_process_output`, terminate with `stop_process`.";
    }
    std::string short_description() const override {
        return "Start a long-running shell process; returns a process_id.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"command", "string", "The command to launch in the background", true},
        };
    }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

class ReadProcessOutputTool : public ITool {
public:
    std::string name()        const override { return "read_process_output"; }
    std::string description() const override {
        return "Read buffered stdout+stderr from a background process. By default returns "
               "everything new since the last read of this process_id; pass `since_offset=0` "
               "to read from the beginning. Returns the data, the next offset to pass back, "
               "the process status (running/exited/killed), and the exit code if it has "
               "exited. Output is ring-buffered -- extremely chatty processes may drop bytes.\n\n"
               "Accepts the same `output_filter_mode` / `_pattern` / `_lines` / `_context` "
               "params as `run_command` (see that tool's description). Default is smart "
               "head_tail truncation of the read window; the full pre-filter window is "
               "always written to `.locus/locus.log` at trace level.";
    }
    std::string short_description() const override {
        return "Read new stdout/stderr from a background process_id.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"process_id",   "integer", "Background process id from run_command_bg",   true},
            {"since_offset", "integer",
             "Byte offset to read from. Omit to continue from the last read.",          false},
            {"output_filter_mode",    "string",
             "How to trim the returned read window. Same modes as `run_command`: "
             "`head_tail` (default), `head`, `tail`, `regex`, `substring`.",
             false},
            {"output_filter_pattern", "string",
             "Required for `regex` / `substring` mode.",
             false},
            {"output_filter_lines",   "integer",
             "Per-side line count for head_tail; total kept lines for "
             "head/tail; max matches for regex/substring. 0 = workspace default.",
             false},
            {"output_filter_context", "integer",
             "Lines of -B/-A context per match for regex/substring (0..10).",
             false},
        };
    }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

class StopProcessTool : public ITool {
public:
    std::string name()        const override { return "stop_process"; }
    std::string description() const override {
        return "Terminate a background process tree and capture its final exit code. "
               "The entry stays listed (status=killed) until removed.";
    }
    std::string short_description() const override {
        return "Terminate a background process tree by process_id.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"process_id", "integer", "Background process id from run_command_bg", true},
        };
    }
    bool available(IWorkspaceServices& ws) const override;
    std::string preview(const ToolCall& call) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

class ListProcessesTool : public ITool {
public:
    std::string name()        const override { return "list_processes"; }
    std::string description() const override {
        return "List every background process tracked by this workspace, including ones "
               "that have already exited. Returns id, command, status, started_at, exit_code, "
               "and bytes of unread output.";
    }
    std::string short_description() const override {
        return "List every tracked background process for this workspace.";
    }
    std::vector<ToolParam> params() const override { return {}; }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    bool available(IWorkspaceServices& ws) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus

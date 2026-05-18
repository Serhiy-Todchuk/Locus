#pragma once

#include "tool.h"

namespace locus {

class RunCommandTool : public ITool {
public:
    std::string name()        const override { return "run_command"; }
    std::string description() const override {
        return "Execute a shell command synchronously in the workspace directory and "
               "return stdout/stderr + exit code. Use this for commands you expect to "
               "terminate -- builds, tests, scripts, queries. Default timeout 30 minutes. "
               "For dev servers, watchers, and anything that streams output without "
               "terminating, use `run_command_bg` instead.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"command",    "string",  "The command to execute", true},
            {"timeout_ms", "integer",
             "Timeout in milliseconds (default 1800000 = 30 minutes).", false},
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
               "exited. Output is ring-buffered -- extremely chatty processes may drop bytes.";
    }
    std::vector<ToolParam> params() const override {
        return {
            {"process_id",   "integer", "Background process id from run_command_bg",   true},
            {"since_offset", "integer",
             "Byte offset to read from. Omit to continue from the last read.",          false},
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
    std::vector<ToolParam> params() const override { return {}; }
    ToolApprovalPolicy approval_policy() const override { return ToolApprovalPolicy::auto_approve; }
    bool available(IWorkspaceServices& ws) const override;
    ToolResult  execute(const ToolCall& call, IWorkspaceServices& ws,
                        const std::atomic<bool>* cancel_flag = nullptr) override;
};

} // namespace locus

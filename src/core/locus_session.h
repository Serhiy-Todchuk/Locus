#pragma once

#include "agent/agent_core.h"
#include "llm_client.h"
#include "tool_registry.h"
#include "workspace.h"

#include <filesystem>
#include <memory>

namespace locus {

// One bundled lifecycle for everything that hangs off an open workspace:
// `Workspace` → `ILLMClient` → `ToolRegistry` → `AgentCore`. Frontends (CLI,
// GUI, future Crow server) construct one of these and forget about wiring
// order — destruction unwinds in reverse declaration order automatically.
//
// Threading: the AgentCore thread is started at the end of the constructor
// and stopped at the top of the destructor, so the bundle is safe to drop
// from the calling thread even mid-turn.
//
// LLM config resolution layers (lowest to highest priority — each non-empty
// value overrides the previous):
//   1. defaults baked into LLMConfig
//   2. workspace .locus/config.json   (llm_endpoint / llm_model / ... )
//   3. seed_config passed in by the caller (CLI args, GUI startup args)
//   4. live `query_model_info` from the server (only fills *empty* fields)
//
// This way the CLI's --model flag still wins over a stale value in
// config.json, and a workspace switch in the GUI still picks up that
// workspace's saved endpoint without losing user-supplied overrides.
class LocusSession {
public:
    // Throws std::runtime_error on failure (Workspace ctor, model probe).
    LocusSession(const std::filesystem::path& ws_path,
                 LLMConfig                    seed_config = {});

    ~LocusSession();

    LocusSession(const LocusSession&)            = delete;
    LocusSession& operator=(const LocusSession&) = delete;

    Workspace&     workspace() { return *workspace_; }
    AgentCore&     agent()     { return *agent_; }
    IToolRegistry& tools()     { return *tools_; }
    ILLMClient&    llm()       { return *llm_; }

    const LLMConfig& llm_config() const { return llm_config_; }

private:
    // Declaration order = destruction order (reversed). Workspace must outlive
    // everything else; AgentCore must die first so its agent thread joins
    // before any of the systems it touches go away.
    std::unique_ptr<Workspace>     workspace_;
    std::unique_ptr<ILLMClient>    llm_;
    std::unique_ptr<ToolRegistry>  tools_;
    std::unique_ptr<AgentCore>     agent_;

    LLMConfig llm_config_;
};

} // namespace locus
